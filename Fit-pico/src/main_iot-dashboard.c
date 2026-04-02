#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/apps/mdns.h"

#include "hardware/flash.h"
#include "hardware/sync.h"

#include "config.h"

#define HTTP_PORT 80
#define MAX_USERS           8
#define FLASH_MAGIC         0xFD5A1234U
#define FLASH_USER_SIZE     512    // 2 flash pages (각 256 bytes)
#define FLASH_USER_OFFSET   (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

typedef struct __attribute__((packed)) {
    char    uid[12];      // "A3:B2:C1:D0\0"
    char    name[20];
    uint8_t weight_kg;
    uint8_t goal_sets;
    uint8_t _pad[2];
} user_t;   // 36 bytes

typedef struct {
    uint32_t magic;
    uint8_t  count;
    uint8_t  _pad[3];
    user_t   users[MAX_USERS];      // 8 * 36 = 288 bytes
    uint8_t  _fill[512 - 8 - 288];  // = 216 bytes, 총 512
} flash_user_block_t;

typedef struct {
    int      today_reps;
    int      today_sets;
    int      total_reps;
    int      total_sets;
    int      reps_at_login;   // 로그인 시점 g_daily_reps 스냅샷
    int      sets_at_login;
} user_stats_t;

#define HTTP_REQ_BUF_SIZE 2048
#define DASHBOARD_HOSTNAME "fitpico-dashboard"
#define SPEED_HISTORY 5
#define DASHBOARD_TOPIC_COUNT 7

static mqtt_client_t *g_mqtt_client = NULL;
static bool g_mqtt_ready = false;
static bool g_wifi_ready = false;
static bool g_mqtt_connecting = false;
static uint32_t g_last_mqtt_attempt_ms = 0;

static char g_cur_topic[64] = {0};
static char g_payload[256] = {0};
static int g_pay_len = 0;

static int g_reps = 0;
static int g_sets = 0;
static bool g_active = false;
static bool g_tracking = false;
static bool g_sensing = true;

static int g_speed_ms = 0;
static char g_warn[8] = "---";
static int g_speed_rep = 0;

static int g_rest_sec = 0;
static int g_rest_after = 0;

static int g_daily_reps = 0;
static int g_daily_sets = 0;
static uint32_t g_session_active_sec = 0;
static uint32_t g_daily_active_sec = 0;

static uint32_t g_last_sensor_seen_ms = 0;
static uint32_t g_last_display_seen_ms = 0;

static int g_speed_hist[SPEED_HISTORY] = {0};
static int g_hist_idx = 0;
static int g_hist_count = 0;

static struct tcp_pcb *g_http_pcb = NULL;

static user_t       g_users[MAX_USERS]      = {0};
static user_stats_t g_stats[MAX_USERS]      = {0};
static int          g_user_count            = 0;
static int          g_current_user          = -1;   // -1 = 미로그인
static bool         g_scan_mode             = false;
static char         g_pending_uid[12]       = {0};
static size_t       g_dashboard_subscribe_index = 0;

static const char *const g_dashboard_topics[DASHBOARD_TOPIC_COUNT] = {
    TOPIC_COUNT,
    TOPIC_SPEED,
    TOPIC_REST,
    TOPIC_DAILY,
    TOPIC_SENSOR_STATUS,
    TOPIC_DISPLAY_STATUS,
    TOPIC_RFID_UID,
};

static const char DASHBOARD_HTML[] =
"<!doctype html>\n"
"<html lang=\"ko\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>핏피코 대시보드</title>\n"
"<style>\n"
":root{color-scheme:light;background:#f7f5ef;color:#122027;--card:#fffdf8;--line:#d9d2c4;--accent:#0d9488;--warn:#ca8a04;--danger:#dc2626;--muted:#5b6470;--ok:#15803d;--paused:#2563eb;--monitor:#0f766e}\n"
"*{box-sizing:border-box}body{margin:0;font-family:ui-rounded,system-ui,-apple-system,sans-serif;background:radial-gradient(circle at top,#fff7e8 0,#f7f5ef 48%,#efece2 100%);color:#122027}\n"
".wrap{max-width:1080px;margin:0 auto;padding:24px 16px 40px}.hero{display:flex;justify-content:space-between;gap:16px;align-items:flex-start;margin-bottom:20px}\n"
"h1{margin:0;font-size:clamp(28px,5vw,46px);letter-spacing:-0.04em}.sub{margin:8px 0 0;color:var(--muted)}\n"
".pill{display:inline-flex;align-items:center;gap:8px;padding:10px 14px;border:1px solid var(--line);border-radius:999px;background:rgba(255,255,255,.7);backdrop-filter:blur(8px);font-weight:700}\n"
".dot{width:12px;height:12px;border-radius:50%;background:#94a3b8}.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:16px}\n"
".card{grid-column:span 12;background:var(--card);border:1px solid var(--line);border-radius:24px;padding:18px;box-shadow:0 10px 30px rgba(18,32,39,.06)}\n"
".kpi{grid-column:span 3}.wide{grid-column:span 6}.full{grid-column:span 12}.label{font-size:13px;color:var(--muted);text-transform:uppercase;letter-spacing:.08em}.value{margin-top:10px;font-size:clamp(28px,4vw,44px);font-weight:800}\n"
".status{font-size:18px;font-weight:800}.status.active{color:var(--ok)}.status.paused{color:var(--paused)}.status.monitor{color:var(--monitor)}.status.warn{color:var(--warn)}.status.danger{color:var(--danger)}\n"
".meta{margin-top:8px;color:var(--muted)}.list{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}.chip{padding:8px 12px;border-radius:999px;background:#f3efe6;border:1px solid var(--line);font-weight:600}\n"
".history{display:flex;gap:10px;align-items:flex-end;height:140px;margin-top:18px}.bar{flex:1;min-width:24px;background:linear-gradient(180deg,#14b8a6,#0f766e);border-radius:12px 12px 4px 4px;position:relative}\n"
".bar span{position:absolute;bottom:-26px;left:50%;transform:translateX(-50%);font-size:12px;color:var(--muted)}\n"
".controls{display:flex;flex-wrap:wrap;gap:12px;align-items:center;margin-top:12px}.btn{border:0;border-radius:999px;padding:12px 18px;font-weight:800;cursor:pointer;background:#0f766e;color:#fff}.btn.stop{background:#b91c1c}.btn:disabled{opacity:.5;cursor:not-allowed}.weight-wrap{display:flex;align-items:center;gap:10px;padding:10px 14px;border:1px solid var(--line);border-radius:999px;background:#f8f3e8;font-weight:700}.weight-wrap input{width:88px;border:0;background:transparent;font:inherit;outline:none}\n"
".rfid-form input{padding:10px 12px;border-radius:12px;border:1px solid var(--line);font-size:16px;background:#fffdf8}.rfid-form input[type=number]{width:110px}.rfid-panel{margin-top:16px;background:#f3efe6}\n"
".footer{margin-top:18px;color:var(--muted);font-size:14px}@media (max-width:820px){.kpi,.wide{grid-column:span 12}.hero{flex-direction:column}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"wrap\">\n"
"<div class=\"hero\">\n"
"<div><h1>핏피코 대시보드</h1><p class=\"sub\">웹에서 운동 저장을 시작·종료하고, 보드 상태와 추정 칼로리를 확인합니다.</p></div>\n"
"<div class=\"pill\"><span class=\"dot\" id=\"net-dot\"></span><span id=\"net-text\">연결 확인 중...</span></div>\n"
"</div>\n"
"<div class=\"grid\">\n"
"<section class=\"card wide\"><div class=\"label\">운동 상태</div><div class=\"value status\" id=\"status-text\">-</div><div class=\"meta\" id=\"status-meta\">MQTT 데이터를 기다리는 중...</div></section>\n"
"<section class=\"card kpi\"><div class=\"label\">반복 횟수</div><div class=\"value\" id=\"reps\">0</div></section>\n"
"<section class=\"card kpi\"><div class=\"label\">세트</div><div class=\"value\" id=\"sets\">0 / 0</div></section>\n"
"<section class=\"card kpi\"><div class=\"label\">최근 속도</div><div class=\"value\" id=\"speed\">0 ms</div></section>\n"
"<section class=\"card full\"><div class=\"label\">운동 제어</div><div class=\"controls\"><button class=\"btn\" id=\"start-btn\">운동 시작</button><button class=\"btn stop\" id=\"stop-btn\">운동 종료</button><label class=\"weight-wrap\">체중(kg)<input id=\"weight\" type=\"number\" min=\"1\" max=\"300\" step=\"0.1\" value=\"70\"></label></div><div class=\"meta\" id=\"control-meta\">버튼을 누르면 센서 보드의 저장 모드가 전환됩니다.</div></section>\n"
"<section class=\"card kpi\"><div class=\"label\">현재 세션 kcal</div><div class=\"value\" id=\"session-kcal\">0.0 kcal</div></section>\n"
"<section class=\"card kpi\"><div class=\"label\">오늘 누적 kcal</div><div class=\"value\" id=\"daily-kcal\">0.0 kcal</div></section>\n"
"<section class=\"card wide\"><div class=\"label\">보드 연결 상태</div><div class=\"list\"><div class=\"chip\" id=\"sensor-board\">센서 보드: 확인 중</div><div class=\"chip\" id=\"display-board\">디스플레이 보드: 확인 중</div></div></section>\n"
"<section class=\"card full\"><div class=\"label\">경고 및 누적 정보</div><div class=\"list\"><div class=\"chip\" id=\"warn\">경고: ---</div><div class=\"chip\" id=\"rest\">휴식: -</div><div class=\"chip\" id=\"daily\">일일 누적: 0회 / 0세트</div><div class=\"chip\" id=\"mode-chip\">센서 모드: 확인 중</div></div></section>\n"
"<section class=\"card full\"><div class=\"label\">최근 속도 기록</div><div class=\"history\" id=\"history\"></div><div class=\"footer\">0.5초마다 자동으로 갱신됩니다.</div></section>\n"
"<section class=\"card full\" id=\"rfid-section\"><div class=\"label\">RFID 사용자</div><div id=\"current-user-name\" class=\"value\" style=\"font-size:28px\">-</div><div class=\"controls\" style=\"margin-top:16px\"><button class=\"btn\" id=\"scan-btn\">카드 갖다대기 (등록)</button></div><div id=\"reg-form\" class=\"rfid-form\" style=\"display:none;margin-top:16px\"><div class=\"controls\"><input id=\"reg-name\" placeholder=\"이름\"><input id=\"reg-weight\" type=\"number\" placeholder=\"체중(kg)\" min=\"1\" max=\"300\"><input id=\"reg-goal\" type=\"number\" placeholder=\"목표세트\" min=\"1\" max=\"20\"><button class=\"btn\" id=\"reg-submit\">등록</button></div><div id=\"reg-uid-label\" class=\"meta\" style=\"margin-top:8px\"></div></div><div id=\"rfid-meta\" class=\"meta\" style=\"margin-top:8px\">카드를 리더기에 태그하면 사용자가 전환됩니다.</div><div class=\"card rfid-panel\"><div class=\"label\">오늘 세션 통계</div><div class=\"list\"><div class=\"chip\" id=\"user-session-reps\">렙: -</div><div class=\"chip\" id=\"user-session-sets\">세트: -</div><div class=\"chip\" id=\"user-total-reps\">누적 렙: -</div><div class=\"chip\" id=\"user-total-sets\">누적 세트: -</div><div class=\"chip\" id=\"user-goal\">목표: -</div></div></div></section>\n"
"</div>\n"
"</div>\n"
"<script>\n"
"const WEIGHT_KEY='fitpico-weight-kg';\n"
"function esc(v){return String(v)}\n"
"function loadWeight(){const saved=localStorage.getItem(WEIGHT_KEY);return saved?saved:'70';}\n"
"function saveWeight(v){localStorage.setItem(WEIGHT_KEY,String(v));}\n"
"function calcKcal(weightKg,activeSec){const minutes=activeSec/60.0;return 8.0*3.5*weightKg/200.0*minutes;}\n"
"function fmtKcal(v){return Number.isFinite(v)?v.toFixed(1)+' kcal':'0.0 kcal';}\n"
"function statusInfo(data){if(data.active){if(data.warn==='slow')return{text:'운동 중',cls:'danger'};if(data.warn==='fast')return{text:'운동 중',cls:'warn'};return{text:'운동 중',cls:'active'};}if(data.tracking)return{text:'중지됨',cls:'paused'};return{text:'감지 중(저장 안 함)',cls:'monitor'};}\n"
"function renderBars(history){const host=document.getElementById('history');host.innerHTML='';if(!history.length){host.innerHTML='<div class=\"meta\">아직 속도 기록이 없습니다.</div>';return;}const max=Math.max(...history,1);history.forEach(v=>{const bar=document.createElement('div');bar.className='bar';bar.style.height=Math.max(24,Math.round((v/max)*120))+'px';const label=document.createElement('span');label.textContent=v+'ms';bar.appendChild(label);host.appendChild(bar);});}\n"
"function boardLabel(name,status,seconds,extra){if(status==='checking')return name+': 확인 중';if(status==='online')return name+': 온라인 ('+seconds+'초 전'+(extra?' · '+extra:'')+')';return name+': 오프라인';}\n"
"async function refreshStats(){try{const res=await fetch('/api/stats/current',{cache:'no-store'});const s=await res.json();if(s.logged_in){document.getElementById('user-session-reps').textContent='렙: '+s.session_reps;document.getElementById('user-session-sets').textContent='세트: '+s.session_sets;document.getElementById('user-total-reps').textContent='누적 렙: '+s.total_reps;document.getElementById('user-total-sets').textContent='누적 세트: '+s.total_sets;document.getElementById('user-goal').textContent='목표 '+s.goal_sets+'세트 '+(s.goal_achieved?'✓ 달성':'진행중');}else{document.getElementById('user-session-reps').textContent='렙: -';document.getElementById('user-session-sets').textContent='세트: -';document.getElementById('user-total-reps').textContent='누적 렙: -';document.getElementById('user-total-sets').textContent='누적 세트: -';document.getElementById('user-goal').textContent='목표: -';}}catch(e){}}\n"
"async function sendControl(command){const meta=document.getElementById('control-meta');meta.textContent='명령 전송 중...';try{const res=await fetch('/api/control/'+command,{method:'POST'});const data=await res.json();if(!res.ok||!data.ok)throw new Error(data.error||'request_failed');meta.textContent=command==='start'?'운동 시작 명령을 보냈습니다.':'운동 종료 명령을 보냈습니다.';refresh();}catch(err){meta.textContent='명령 전송 실패: '+err.message;}}\n"
"function updateRfidUi(data){const form=document.getElementById('reg-form');const uidLabel=document.getElementById('reg-uid-label');const meta=document.getElementById('rfid-meta');const submit=document.getElementById('reg-submit');if(data.pending_uid&&data.pending_uid.length>0){form.style.display='block';uidLabel.textContent='카드 UID: '+data.pending_uid;meta.textContent='등록 정보를 입력하고 등록 버튼을 누르세요.';submit.disabled=false;return;}if(data.scan_mode){form.style.display='block';uidLabel.textContent='카드 UID: 대기 중';meta.textContent=data.sensor_status==='online'?'카드를 리더기에 태그하세요.':'센서 보드 연결을 확인한 뒤 카드를 태그하세요.';submit.disabled=true;return;}if(form.dataset.waiting==='true'){form.style.display='none';uidLabel.textContent='';submit.disabled=true;}form.dataset.waiting='false';}\n"
"async function refresh(){try{const res=await fetch('/api/status',{cache:'no-store'});const data=await res.json();const weight=parseFloat(document.getElementById('weight').value)||70;document.getElementById('net-text').textContent=data.mqtt_ready?'MQTT 연결됨':'MQTT 대기 중';document.getElementById('net-dot').style.background=data.mqtt_ready?'#16a34a':'#f59e0b';const info=statusInfo(data);const statusEl=document.getElementById('status-text');statusEl.textContent=info.text;statusEl.className='value status '+info.cls;document.getElementById('status-meta').textContent='세션 시간 '+data.session_active_sec+'초 | 최근 반복 '+data.speed_rep+' | 경고 '+data.warn;document.getElementById('reps').textContent=esc(data.reps);document.getElementById('sets').textContent=esc(data.sets)+' / '+esc(data.target_sets);document.getElementById('speed').textContent=esc(data.speed_ms)+' ms';document.getElementById('warn').textContent='경고: '+esc(data.warn);document.getElementById('rest').textContent='휴식: '+(data.rest_sec>0?(esc(data.rest_sec)+'초 (세트 '+esc(data.rest_after)+' 후)'):'-');document.getElementById('daily').textContent='일일 누적: '+esc(data.daily_reps)+'회 / '+esc(data.daily_sets)+'세트';document.getElementById('mode-chip').textContent='센서 모드: '+(data.tracking?'저장 중':'모니터링');document.getElementById('sensor-board').textContent=boardLabel('센서 보드',data.sensor_status,data.sensor_seen_sec,data.tracking?'저장 중':'모니터링');document.getElementById('display-board').textContent=boardLabel('디스플레이 보드',data.display_status,data.display_seen_sec,'표시 중');document.getElementById('session-kcal').textContent=fmtKcal(calcKcal(weight,data.session_active_sec));document.getElementById('daily-kcal').textContent=fmtKcal(calcKcal(weight,data.daily_active_sec));renderBars(data.speed_history||[]);const uname=data.current_user||'';document.getElementById('current-user-name').textContent=uname||'로그인 전';updateRfidUi(data);}catch(err){document.getElementById('net-text').textContent='대시보드 연결 끊김';document.getElementById('net-dot').style.background='#dc2626';}}\n"
"window.addEventListener('load',()=>{const weight=document.getElementById('weight');const regForm=document.getElementById('reg-form');const regSubmit=document.getElementById('reg-submit');weight.value=loadWeight();regSubmit.disabled=true;regForm.dataset.waiting='false';weight.addEventListener('input',()=>{saveWeight(weight.value);refresh();});document.getElementById('start-btn').addEventListener('click',()=>sendControl('start'));document.getElementById('stop-btn').addEventListener('click',()=>sendControl('stop'));document.getElementById('scan-btn').addEventListener('click',async()=>{regForm.style.display='block';regForm.dataset.waiting='true';document.getElementById('reg-uid-label').textContent='카드 UID: 대기 중';document.getElementById('rfid-meta').textContent='카드를 리더기에 태그하세요...';regSubmit.disabled=true;try{await fetch('/api/rfid/scan-mode',{method:'POST'});refresh();}catch(err){document.getElementById('rfid-meta').textContent='등록 대기 시작 실패: '+err.message;}});document.getElementById('reg-submit').addEventListener('click',async()=>{const resp=await fetch('/api/status',{cache:'no-store'});const d=await resp.json();const uid=d.pending_uid;const name=document.getElementById('reg-name').value.trim();const weightValue=parseInt(document.getElementById('reg-weight').value,10)||70;const goal=parseInt(document.getElementById('reg-goal').value,10)||3;if(!uid||!name){alert('이름과 UID를 확인하세요');return;}const res=await fetch('/api/users',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({uid,name,weight:weightValue,goal_sets:goal})});const result=await res.json();if(result.ok){document.getElementById('reg-form').style.display='none';document.getElementById('reg-form').dataset.waiting='false';document.getElementById('reg-name').value='';document.getElementById('reg-weight').value='';document.getElementById('reg-goal').value='';document.getElementById('rfid-meta').textContent=name+' 등록 완료!';document.getElementById('reg-submit').disabled=true;refresh();refreshStats();}else{alert('등록 실패: '+result.error);}});refresh();refreshStats();setInterval(refresh,500);setInterval(refreshStats,1000);});\n"
"</script>\n"
"</body>\n"
"</html>\n";

typedef struct http_state {
    char request[HTTP_REQ_BUF_SIZE];
    size_t request_len;
    char *response;
    size_t response_len;
    size_t response_sent;
} http_state_t;

static int http_content_length(const char *request) {
    const char *header = strstr(request, "Content-Length:");
    if (!header) return 0;
    header += strlen("Content-Length:");
    while (*header == ' ') header++;
    return atoi(header);
}

static bool http_request_complete(const http_state_t *state) {
    const char *body = strstr(state->request, "\r\n\r\n");
    if (!body) return false;

    size_t header_len = (size_t)(body - state->request) + 4;
    int content_length = http_content_length(state->request);
    if (content_length <= 0) return true;

    return state->request_len >= header_len + (size_t)content_length;
}

static int json_int(const char *json, const char *key) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    return atoi(p);
}

static void json_str(const char *json, const char *key, char *out, int sz) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) {
        out[0] = '\0';
        return;
    }
    p += strlen(search);
    int i = 0;
    while (*p && *p != '"' && i < sz - 1) out[i++] = *p++;
    out[i] = '\0';
}

static bool json_bool(const char *json, const char *key, bool default_value) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return default_value;
    p += strlen(search);
    while (*p == ' ') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return default_value;
}

static int speed_avg(void) {
    if (g_hist_count == 0) return 0;
    int sum = 0;
    for (int i = 0; i < g_hist_count; i++) sum += g_speed_hist[i];
    return sum / g_hist_count;
}

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static const char *mqtt_status_name(mqtt_connection_status_t status) {
    switch (status) {
        case MQTT_CONNECT_ACCEPTED: return "ACCEPTED";
        case MQTT_CONNECT_REFUSED_PROTOCOL_VERSION: return "REFUSED_PROTOCOL_VERSION";
        case MQTT_CONNECT_REFUSED_IDENTIFIER: return "REFUSED_IDENTIFIER";
        case MQTT_CONNECT_REFUSED_SERVER: return "REFUSED_SERVER";
        case MQTT_CONNECT_REFUSED_USERNAME_PASS: return "REFUSED_USERNAME_PASS";
        case MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_: return "REFUSED_NOT_AUTHORIZED";
        case MQTT_CONNECT_DISCONNECTED: return "DISCONNECTED";
        case MQTT_CONNECT_TIMEOUT: return "TIMEOUT";
        default: return "UNKNOWN";
    }
}

static uint32_t seconds_since(uint32_t last_seen_ms, uint32_t current_ms) {
    if (last_seen_ms == 0 || current_ms < last_seen_ms) return 9999;
    return (current_ms - last_seen_ms) / 1000;
}

static void mark_sensor_seen(void) {
    g_last_sensor_seen_ms = now_ms();
}

static void mark_display_seen(void) {
    g_last_display_seen_ms = now_ms();
}

static const char *board_status(uint32_t last_seen_ms, uint32_t current_ms) {
    if (last_seen_ms == 0) return "checking";
    if (current_ms < last_seen_ms) return "checking";
    if ((current_ms - last_seen_ms) > BOARD_OFFLINE_TIMEOUT_MS) return "offline";
    return "online";
}

static size_t build_speed_history_json(char *out, size_t out_size) {
    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, out_size - pos, "[");
    for (int i = 0; i < g_hist_count && pos < out_size; i++) {
        int idx = (g_hist_idx - g_hist_count + i + SPEED_HISTORY) % SPEED_HISTORY;
        pos += (size_t)snprintf(out + pos, out_size - pos, "%s%d",
                                i ? "," : "", g_speed_hist[idx]);
    }
    if (pos < out_size) {
        snprintf(out + pos, out_size - pos, "]");
    }
    return pos;
}

static err_t mqtt_send_message(const char *topic, const char *payload) {
    err_t err;
    if (!g_mqtt_ready || !g_mqtt_client) return ERR_CONN;
    cyw43_arch_lwip_begin();
    err = mqtt_publish(g_mqtt_client, topic, payload,
                       (uint16_t)strlen(payload), 0, 0, NULL, NULL);
    cyw43_arch_lwip_end();
    if (err != ERR_OK) {
        printf("[MQTT] Publish request failed on %s: %d\n", topic, err);
    }
    return err;
}

static bool mqtt_client_connected(void) {
    bool connected = false;
    if (!g_mqtt_client) return false;
    cyw43_arch_lwip_begin();
    connected = mqtt_client_is_connected(g_mqtt_client);
    cyw43_arch_lwip_end();
    return connected;
}

static bool publish_control_command(const char *command) {
    char payload[48];
    snprintf(payload, sizeof(payload), "{\"command\":\"%s\"}", command);
    return mqtt_send_message(TOPIC_CONTROL, payload) == ERR_OK;
}

// ─── 플래시 사용자 저장소 ────────────────────────────────────────────────────

static void users_flash_load(void) {
    const flash_user_block_t *block =
        (const flash_user_block_t *)(XIP_BASE + FLASH_USER_OFFSET);
    if (block->magic != FLASH_MAGIC) {
        printf("[FLASH] 저장된 사용자 없음 (magic 불일치)\n");
        return;
    }
    g_user_count = block->count < MAX_USERS ? block->count : MAX_USERS;
    memcpy(g_users, block->users, (size_t)g_user_count * sizeof(user_t));
    printf("[FLASH] 사용자 %d명 로드\n", g_user_count);
}

static void users_flash_save(void) {
    static uint8_t buf[FLASH_USER_SIZE] __attribute__((aligned(4)));
    flash_user_block_t *block = (flash_user_block_t *)buf;

    memset(buf, 0xFF, sizeof(buf));
    block->magic = FLASH_MAGIC;
    block->count = (uint8_t)g_user_count;
    memcpy(block->users, g_users, (size_t)g_user_count * sizeof(user_t));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_USER_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_USER_OFFSET, buf, FLASH_USER_SIZE);
    restore_interrupts(ints);

    printf("[FLASH] 사용자 %d명 저장\n", g_user_count);
}

static void snapshot_current_user_stats(void) {
    if (g_current_user < 0) return;

    user_stats_t *stats = &g_stats[g_current_user];
    int session_reps = g_daily_reps - stats->reps_at_login;
    int session_sets = g_daily_sets - stats->sets_at_login;

    stats->today_reps = session_reps > 0 ? session_reps : 0;
    stats->today_sets = session_sets > 0 ? session_sets : 0;
    stats->total_reps += stats->today_reps;
    stats->total_sets += stats->today_sets;
}

static void publish_user_switch(int user_index, const char *uid) {
    char payload[96];
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uid\":\"%s\","
             "\"weight\":%d,\"goal_sets\":%d}",
             g_users[user_index].name, uid,
             g_users[user_index].weight_kg, g_users[user_index].goal_sets);
    mqtt_send_message(TOPIC_RFID_USER, payload);
}

static void activate_user(int user_index, const char *uid) {
    if (user_index < 0 || user_index >= g_user_count) return;

    if (g_current_user != user_index) {
        snapshot_current_user_stats();
    }

    g_current_user = user_index;
    g_stats[user_index].reps_at_login = g_daily_reps;
    g_stats[user_index].sets_at_login = g_daily_sets;
    g_pending_uid[0] = '\0';
    g_scan_mode = false;

    publish_user_switch(user_index, uid);
    printf("[RFID] 사용자 전환: %s\n", g_users[user_index].name);
}

static void on_publish(void *arg, const char *topic, u32_t tot_len) {
    (void)arg;
    (void)tot_len;
    strncpy(g_cur_topic, topic, sizeof(g_cur_topic) - 1);
    g_cur_topic[sizeof(g_cur_topic) - 1] = '\0';
    g_pay_len = 0;
}

static void on_data(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    (void)arg;
    int copy = len;
    if (g_pay_len + copy >= (int)sizeof(g_payload) - 1) {
        copy = (int)sizeof(g_payload) - 1 - g_pay_len;
    }
    memcpy(g_payload + g_pay_len, data, (size_t)copy);
    g_pay_len += copy;
    if (!(flags & MQTT_DATA_FLAG_LAST)) return;
    g_payload[g_pay_len] = '\0';

    if (strcmp(g_cur_topic, TOPIC_COUNT) == 0) {
        g_reps = json_int(g_payload, "reps");
        g_sets = json_int(g_payload, "sets");
        g_active = json_bool(g_payload, "active", false);
        g_tracking = json_bool(g_payload, "tracking", false);
        g_sensing = json_bool(g_payload, "sensing", true);
        g_session_active_sec = (uint32_t)json_int(g_payload, "session_active_sec");
        mark_sensor_seen();
    } else if (strcmp(g_cur_topic, TOPIC_SPEED) == 0) {
        g_speed_ms = json_int(g_payload, "speed_ms");
        g_speed_rep = json_int(g_payload, "rep");
        json_str(g_payload, "warn", g_warn, sizeof(g_warn));
        g_speed_hist[g_hist_idx] = g_speed_ms;
        g_hist_idx = (g_hist_idx + 1) % SPEED_HISTORY;
        if (g_hist_count < SPEED_HISTORY) g_hist_count++;
    } else if (strcmp(g_cur_topic, TOPIC_REST) == 0) {
        g_rest_sec = json_int(g_payload, "rest_sec");
        g_rest_after = json_int(g_payload, "set");
    } else if (strcmp(g_cur_topic, TOPIC_DAILY) == 0) {
        g_daily_reps = json_int(g_payload, "total_reps");
        g_daily_sets = json_int(g_payload, "total_sets");
        g_daily_active_sec = (uint32_t)json_int(g_payload, "daily_active_sec");
        mark_sensor_seen();
    } else if (strcmp(g_cur_topic, TOPIC_SENSOR_STATUS) == 0) {
        g_active = json_bool(g_payload, "active", g_active);
        g_tracking = json_bool(g_payload, "tracking", g_tracking);
        g_sensing = json_bool(g_payload, "sensing", g_sensing);
        mark_sensor_seen();
    } else if (strcmp(g_cur_topic, TOPIC_DISPLAY_STATUS) == 0) {
        mark_display_seen();
    } else if (strcmp(g_cur_topic, TOPIC_RFID_UID) == 0) {
        char uid[12];
        json_str(g_payload, "uid", uid, sizeof(uid));
        if (uid[0] == '\0') return;

        // 등록 대기 모드: UID 캡처만 하고 사용자 전환 안 함
        if (g_scan_mode) {
            memcpy(g_pending_uid, uid, sizeof(g_pending_uid));
            g_scan_mode = false;
            printf("[RFID] 등록 대기 UID 캡처: %s\n", uid);
            return;
        }

        // 사용자 조회
        for (int i = 0; i < g_user_count; i++) {
            if (strcmp(g_users[i].uid, uid) == 0) {
                activate_user(i, uid);
                return;
            }
        }
        memcpy(g_pending_uid, uid, sizeof(g_pending_uid));
        g_scan_mode = false;
        printf("[RFID] 미등록 카드: %s (대시보드 등록 가능)\n", uid);
    }
}

static void subscribe_next_dashboard_topic(mqtt_client_t *client);

static void on_sub(void *arg, err_t result) {
    mqtt_client_t *client = (mqtt_client_t *)arg;
    size_t topic_index = g_dashboard_subscribe_index == 0 ? 0 : g_dashboard_subscribe_index - 1;
    const char *topic = g_dashboard_topics[topic_index];

    if (result != ERR_OK) {
        printf("[MQTT] 구독 실패 (%s): %d\n", topic, result);
        return;
    }

    printf("[MQTT] 구독 성공 (%s)\n", topic);
    subscribe_next_dashboard_topic(client);
}

static void subscribe_next_dashboard_topic(mqtt_client_t *client) {
    if (g_dashboard_subscribe_index >= DASHBOARD_TOPIC_COUNT) {
        return;
    }

    const char *topic = g_dashboard_topics[g_dashboard_subscribe_index];
    err_t err = mqtt_subscribe(client, topic, 0, on_sub, client);
    if (err != ERR_OK) {
        printf("[MQTT] 구독 요청 실패 (%s): %d\n", topic, err);
        return;
    }

    printf("[MQTT] 구독 요청 (%s)\n", topic);
    g_dashboard_subscribe_index++;
}

static void on_connect(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    (void)arg;
    g_mqtt_connecting = false;
    if (status != MQTT_CONNECT_ACCEPTED) {
        printf("[MQTT] 연결 실패 (%s, %d)\n",
               mqtt_status_name(status), status);
        g_mqtt_ready = false;
        return;
    }
    printf("[MQTT] 브로커 연결 성공 (%s, %d)\n",
           mqtt_status_name(status), status);
    g_mqtt_ready = true;
    mqtt_set_inpub_callback(client, on_publish, on_data, NULL);
    g_dashboard_subscribe_index = 0;
    subscribe_next_dashboard_topic(client);
}

static void mqtt_connect_broker(void) {
    ip_addr_t broker;
    err_t err = ERR_OK;
    g_last_mqtt_attempt_ms = now_ms();
    printf("[MQTT] 연결 요청 -> %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
    if (!ip4addr_aton(MQTT_BROKER_IP, &broker)) {
        printf("[MQTT] 잘못된 브로커 IP: %s\n", MQTT_BROKER_IP);
        return;
    }

    struct mqtt_connect_client_info_t ci = {
        .client_id = "fitpico_dashboard",
        .keep_alive = 60,
    };
    if (MQTT_BROKER_USERNAME[0] != '\0') {
        ci.client_user = MQTT_BROKER_USERNAME;
    }
    if (MQTT_BROKER_PASSWORD[0] != '\0') {
        ci.client_pass = MQTT_BROKER_PASSWORD;
    }

    g_mqtt_connecting = true;
    printf("[MQTT] 인증 사용자: %s\n",
           ci.client_user ? ci.client_user : "(anonymous)");
    cyw43_arch_lwip_begin();
    if (!g_mqtt_client) {
        g_mqtt_client = mqtt_client_new();
    }
    if (g_mqtt_client) {
        err = mqtt_client_connect(g_mqtt_client, &broker, MQTT_BROKER_PORT, on_connect, NULL, &ci);
    } else {
        err = ERR_MEM;
    }
    cyw43_arch_lwip_end();
    printf("[MQTT] 연결 요청 결과: %d\n", err);
    if (err != ERR_OK) {
        g_mqtt_connecting = false;
        if (err == ERR_MEM) {
            printf("[MQTT] 클라이언트 생성 실패\n");
        }
        printf("[MQTT] 연결 요청 즉시 실패: %d\n", err);
    }
}

static void maintain_mqtt_connection(uint32_t current_ms) {
    if (!g_wifi_ready) return;
    if (g_mqtt_ready && !mqtt_client_connected()) {
        printf("[MQTT] 연결 상태 유실, 재연결 예약\n");
        g_mqtt_ready = false;
    }
    if (g_mqtt_ready || g_mqtt_connecting) return;
    if ((current_ms - g_last_mqtt_attempt_ms) < MQTT_RECONNECT_INTERVAL_MS) return;
    mqtt_connect_broker();
}

static int build_status_json(char *out, size_t out_size) {
    char history[96] = {0};
    uint32_t current_ms = now_ms();
    uint32_t sensor_seen_sec = seconds_since(g_last_sensor_seen_ms, current_ms);
    uint32_t display_seen_sec = seconds_since(g_last_display_seen_ms, current_ms);
    const char *sensor_state = board_status(g_last_sensor_seen_ms, current_ms);
    const char *display_state = board_status(g_last_display_seen_ms, current_ms);

    build_speed_history_json(history, sizeof(history));

    return snprintf(
        out, out_size,
        "{"
        "\"mqtt_ready\":%s,"
        "\"active\":%s,"
        "\"tracking\":%s,"
        "\"sensing\":%s,"
        "\"sensor_mode\":\"%s\","
        "\"reps\":%d,"
        "\"sets\":%d,"
        "\"target_sets\":%d,"
        "\"speed_ms\":%d,"
        "\"speed_rep\":%d,"
        "\"warn\":\"%s\","
        "\"rest_sec\":%d,"
        "\"rest_after\":%d,"
        "\"daily_reps\":%d,"
        "\"daily_sets\":%d,"
        "\"session_active_sec\":%u,"
        "\"daily_active_sec\":%u,"
        "\"avg_speed\":%d,"
        "\"sensor_status\":\"%s\","
        "\"sensor_online\":%s,"
        "\"sensor_seen_sec\":%u,"
        "\"display_status\":\"%s\","
        "\"display_online\":%s,"
        "\"display_seen_sec\":%u,"
        "\"speed_history\":%s,"
        "\"current_user\":\"%s\","
        "\"scan_mode\":%s,"
        "\"pending_uid\":\"%s\""
        "}",
        g_mqtt_ready ? "true" : "false",
        g_active ? "true" : "false",
        g_tracking ? "true" : "false",
        g_sensing ? "true" : "false",
        g_tracking ? "tracking" : "monitoring",
        g_reps,
        g_sets,
        TARGET_SETS,
        g_speed_ms,
        g_speed_rep,
        g_warn,
        g_rest_sec,
        g_rest_after,
        g_daily_reps,
        g_daily_sets,
        (unsigned)g_session_active_sec,
        (unsigned)g_daily_active_sec,
        speed_avg(),
        sensor_state,
        strcmp(sensor_state, "online") == 0 ? "true" : "false",
        (unsigned)sensor_seen_sec,
        display_state,
        strcmp(display_state, "online") == 0 ? "true" : "false",
        (unsigned)display_seen_sec,
        history,
        g_current_user >= 0 ? g_users[g_current_user].name : "",
        g_scan_mode ? "true" : "false",
        g_pending_uid
    );
}

static void http_free_state(http_state_t *state) {
    if (!state) return;
    free(state->response);
    free(state);
}

static err_t http_close(struct tcp_pcb *tpcb, http_state_t *state) {
    if (!tpcb) {
        http_free_state(state);
        return ERR_OK;
    }

    if (tpcb->unsent != NULL || tpcb->unacked != NULL) {
        return ERR_MEM;
    }

    tcp_arg(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);

    err_t err = tcp_close(tpcb);
    if (err == ERR_OK) {
        http_free_state(state);
    }
    return err;
}

static err_t http_prepare_response(http_state_t *state, const char *status_line,
                                   const char *content_type, const char *body) {
    char header[256];
    int header_len = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Content-Length: %u\r\n\r\n",
        status_line, content_type, (unsigned)strlen(body)
    );

    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        return ERR_VAL;
    }

    size_t body_len = strlen(body);
    size_t total_len = (size_t)header_len + body_len;
    char *response = (char *)malloc(total_len);
    if (!response) {
        return ERR_MEM;
    }

    memcpy(response, header, (size_t)header_len);
    memcpy(response + header_len, body, body_len);

    free(state->response);
    state->response = response;
    state->response_len = total_len;
    state->response_sent = 0;
    return ERR_OK;
}

static err_t http_flush_response(struct tcp_pcb *tpcb, http_state_t *state) {
    while (state->response_sent < state->response_len) {
        u16_t sndbuf = tcp_sndbuf(tpcb);
        if (sndbuf == 0) break;

        size_t remaining = state->response_len - state->response_sent;
        u16_t chunk_len = (u16_t)(remaining < sndbuf ? remaining : sndbuf);
        err_t err = tcp_write(
            tpcb,
            state->response + state->response_sent,
            chunk_len,
            TCP_WRITE_FLAG_COPY
        );
        if (err == ERR_MEM) break;
        if (err != ERR_OK) return err;

        state->response_sent += chunk_len;
    }

    err_t err = tcp_output(tpcb);
    if (err == ERR_MEM) {
        return ERR_OK;
    }
    return err;
}

static err_t http_send_response(struct tcp_pcb *tpcb, http_state_t *state,
                                const char *status_line, const char *content_type,
                                const char *body) {
    err_t err = http_prepare_response(state, status_line, content_type, body);
    if (err != ERR_OK) return err;
    return http_flush_response(tpcb, state);
}

static err_t http_send_checked_response(struct tcp_pcb *tpcb, http_state_t *state,
                                        const char *status_line, const char *content_type,
                                        const char *body) {
    err_t err = http_send_response(tpcb, state, status_line, content_type, body);
    if (err != ERR_OK) {
        printf("[HTTP] 응답 전송 실패 (%d, %s)\n", err, status_line);
    }
    return err;
}

static err_t http_send_not_found(struct tcp_pcb *tpcb, http_state_t *state) {
    return http_send_checked_response(tpcb, state, "404 Not Found",
                                      "text/plain; charset=utf-8", "Not Found");
}

static err_t http_send_control_result(struct tcp_pcb *tpcb, http_state_t *state,
                                      bool ok, const char *message) {
    char body[128];
    snprintf(body, sizeof(body),
             "{\"ok\":%s,\"error\":\"%s\"}",
             ok ? "true" : "false", message);
    return http_send_checked_response(
        tpcb,
        state,
        ok ? "200 OK" : "503 Service Unavailable",
        "application/json; charset=utf-8",
        body
    );
}

static err_t http_send_status(struct tcp_pcb *tpcb, http_state_t *state) {
    char json[768];
    build_status_json(json, sizeof(json));
    return http_send_checked_response(
        tpcb, state, "200 OK", "application/json; charset=utf-8", json
    );
}

static err_t http_send_dashboard(struct tcp_pcb *tpcb, http_state_t *state) {
    return http_send_checked_response(
        tpcb, state, "200 OK", "text/html; charset=utf-8", DASHBOARD_HTML
    );
}

static err_t http_finish_response(struct tcp_pcb *tpcb, http_state_t *state, err_t response_err) {
    if (response_err != ERR_OK) {
        tcp_arg(tpcb, NULL);
        tcp_poll(tpcb, NULL, 0);
        tcp_sent(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
        http_free_state(state);
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    if (state->response_sent >= state->response_len) {
        err_t close_err = http_close(tpcb, state);
        if (close_err == ERR_OK || close_err == ERR_MEM) {
            return ERR_OK;
        }
        printf("[HTTP] 연결 종료 실패 (%d)\n", close_err);
        tcp_arg(tpcb, NULL);
        tcp_poll(tpcb, NULL, 0);
        tcp_sent(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
        http_free_state(state);
        tcp_abort(tpcb);
        return ERR_ABRT;
    }

    return ERR_OK;
}

// ─── 사용자 관리 API 핸들러 ──────────────────────────────────────────────────

static err_t handle_get_users(struct tcp_pcb *tpcb, http_state_t *state) {
    char body[1024];
    size_t pos = 0;
    pos += (size_t)snprintf(body + pos, sizeof(body) - pos, "[");
    for (int i = 0; i < g_user_count && pos < sizeof(body); i++) {
        pos += (size_t)snprintf(
            body + pos, sizeof(body) - pos,
            "%s{\"uid\":\"%s\",\"name\":\"%s\","
            "\"weight\":%d,\"goal_sets\":%d}",
            i ? "," : "",
            g_users[i].uid, g_users[i].name,
            g_users[i].weight_kg, g_users[i].goal_sets
        );
    }
    snprintf(body + pos, sizeof(body) - pos, "]");
    return http_send_checked_response(
        tpcb, state, "200 OK", "application/json; charset=utf-8", body
    );
}

static err_t handle_post_user(struct tcp_pcb *tpcb, http_state_t *state, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return http_send_not_found(tpcb, state);
    }
    body += 4;

    if (g_user_count >= MAX_USERS) {
        return http_send_control_result(tpcb, state, false, "max_users_reached");
    }

    user_t u = {0};
    json_str(body, "uid", u.uid, sizeof(u.uid));
    json_str(body, "name", u.name, sizeof(u.name));
    u.weight_kg = (uint8_t)json_int(body, "weight");
    u.goal_sets = (uint8_t)json_int(body, "goal_sets");

    if (u.uid[0] == '\0' || u.name[0] == '\0') {
        return http_send_control_result(tpcb, state, false, "missing_fields");
    }

    for (int i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].uid, u.uid) == 0) {
            return http_send_control_result(tpcb, state, false, "uid_exists");
        }
    }

    int new_user_index = g_user_count;
    g_users[g_user_count++] = u;
    users_flash_save();
    activate_user(new_user_index, u.uid);
    printf("[USER] 등록: %s (%s)\n", u.name, u.uid);
    return http_send_control_result(tpcb, state, true, "");
}

static err_t handle_get_current_user(struct tcp_pcb *tpcb, http_state_t *state) {
    char body[128];
    if (g_current_user < 0) {
        snprintf(body, sizeof(body), "{\"logged_in\":false}");
    } else {
        user_t *u = &g_users[g_current_user];
        snprintf(body, sizeof(body),
                 "{\"logged_in\":true,\"name\":\"%s\","
                 "\"weight\":%d,\"goal_sets\":%d}",
                 u->name, u->weight_kg, u->goal_sets);
    }
    return http_send_checked_response(
        tpcb, state, "200 OK", "application/json; charset=utf-8", body
    );
}

static err_t handle_scan_mode(struct tcp_pcb *tpcb, http_state_t *state) {
    g_scan_mode = true;
    g_pending_uid[0] = '\0';
    printf("[RFID] 등록 대기 모드 진입\n");
    return http_send_control_result(tpcb, state, true, "");
}

static err_t handle_get_current_stats(struct tcp_pcb *tpcb, http_state_t *state) {
    char body[256];
    if (g_current_user < 0) {
        snprintf(body, sizeof(body), "{\"logged_in\":false}");
    } else {
        user_t *u = &g_users[g_current_user];
        user_stats_t *s = &g_stats[g_current_user];
        int session_reps = g_daily_reps - s->reps_at_login;
        int session_sets = g_daily_sets - s->sets_at_login;
        snprintf(body, sizeof(body),
                 "{\"logged_in\":true,\"name\":\"%s\","
                 "\"session_reps\":%d,\"session_sets\":%d,"
                 "\"total_reps\":%d,\"total_sets\":%d,"
                 "\"goal_sets\":%d,\"goal_achieved\":%s,"
                 "\"pending_uid\":\"%s\"}",
                 u->name,
                 session_reps < 0 ? 0 : session_reps,
                 session_sets < 0 ? 0 : session_sets,
                 s->total_reps + (session_reps > 0 ? session_reps : 0),
                 s->total_sets + (session_sets > 0 ? session_sets : 0),
                 u->goal_sets,
                 session_sets >= u->goal_sets ? "true" : "false",
                 g_pending_uid);
    }
    return http_send_checked_response(
        tpcb, state, "200 OK", "application/json; charset=utf-8", body
    );
}

static err_t http_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)len;
    http_state_t *state = (http_state_t *)arg;
    if (!state) return ERR_OK;
    return http_finish_response(tpcb, state, http_flush_response(tpcb, state));
}

static err_t http_poll(void *arg, struct tcp_pcb *tpcb) {
    http_state_t *state = (http_state_t *)arg;
    if (!state) return ERR_OK;
    return http_finish_response(tpcb, state, http_flush_response(tpcb, state));
}

static err_t http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    http_state_t *state = (http_state_t *)arg;
    if (err != ERR_OK || p == NULL) {
        return http_close(tpcb, state);
    }

    tcp_recved(tpcb, p->tot_len);

    size_t copy = p->tot_len;
    if (copy > sizeof(state->request) - 1 - state->request_len) {
        copy = sizeof(state->request) - 1 - state->request_len;
    }
    pbuf_copy_partial(p, state->request + state->request_len, copy, 0);
    state->request_len += copy;
    state->request[state->request_len] = '\0';
    pbuf_free(p);

    if (!http_request_complete(state)) {
        return ERR_OK;
    }

    err_t response_err = ERR_OK;

    if (strncmp(state->request, "GET /api/status ", 16) == 0) {
        response_err = http_send_status(tpcb, state);
    } else if (strncmp(state->request, "POST /api/control/start ", 24) == 0) {
        response_err = http_send_control_result(tpcb, state, publish_control_command("start"),
                                                g_mqtt_ready ? "" : "mqtt_not_ready");
    } else if (strncmp(state->request, "POST /api/control/stop ", 23) == 0) {
        response_err = http_send_control_result(tpcb, state, publish_control_command("stop"),
                                                g_mqtt_ready ? "" : "mqtt_not_ready");
    } else if (strncmp(state->request, "GET /api/users/current ", 23) == 0) {
        response_err = handle_get_current_user(tpcb, state);
    } else if (strncmp(state->request, "GET /api/users ", 15) == 0) {
        response_err = handle_get_users(tpcb, state);
    } else if (strncmp(state->request, "POST /api/users ", 16) == 0) {
        response_err = handle_post_user(tpcb, state, state->request);
    } else if (strncmp(state->request, "POST /api/rfid/scan-mode ", 25) == 0) {
        response_err = handle_scan_mode(tpcb, state);
    } else if (strncmp(state->request, "GET /api/stats/current ", 23) == 0) {
        response_err = handle_get_current_stats(tpcb, state);
    } else if (strncmp(state->request, "GET / ", 6) == 0 ||
               strncmp(state->request, "GET /HTTP", 9) == 0 ||
               strncmp(state->request, "GET /index.html ", 16) == 0) {
        response_err = http_send_dashboard(tpcb, state);
    } else {
        response_err = http_send_not_found(tpcb, state);
    }

    return http_finish_response(tpcb, state, response_err);
}

static void http_err(void *arg, err_t err) {
    (void)err;
    http_state_t *state = (http_state_t *)arg;
    http_free_state(state);
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) return err;

    http_state_t *state = (http_state_t *)calloc(1, sizeof(http_state_t));
    if (!state) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    tcp_arg(newpcb, state);
    tcp_poll(newpcb, http_poll, 2);
    tcp_sent(newpcb, http_sent);
    tcp_recv(newpcb, http_recv);
    tcp_err(newpcb, http_err);
    return ERR_OK;
}

static bool http_server_init(void) {
    g_http_pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!g_http_pcb) {
        printf("[HTTP] PCB 생성 실패\n");
        return false;
    }
    if (tcp_bind(g_http_pcb, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) {
        printf("[HTTP] 포트 %d bind 실패\n", HTTP_PORT);
        tcp_close(g_http_pcb);
        g_http_pcb = NULL;
        return false;
    }
    g_http_pcb = tcp_listen_with_backlog(g_http_pcb, 2);
    if (!g_http_pcb) {
        printf("[HTTP] listen 실패\n");
        return false;
    }
    tcp_accept(g_http_pcb, http_accept);
    printf("[HTTP] 서버 시작: port %d\n", HTTP_PORT);
    return true;
}

static bool connect_wifi_with_fallbacks(void) {
    const uint32_t auth_modes[] = {
        CYW43_AUTH_WPA2_AES_PSK,
        CYW43_AUTH_WPA2_MIXED_PSK,
        CYW43_AUTH_WPA3_WPA2_AES_PSK,
    };
    const char *auth_names[] = {
        "WPA2_AES",
        "WPA2_MIXED",
        "WPA3_WPA2",
    };

    for (size_t i = 0; i < sizeof(auth_modes) / sizeof(auth_modes[0]); i++) {
        printf("[WiFi] %s 연결 시도...\n", auth_names[i]);
        int err = cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, auth_modes[i], 15000
        );
        if (err == 0) {
            printf("[WiFi] 연결 성공 (%s)\n", auth_names[i]);
            return true;
        }
        printf("[WiFi] 연결 실패 (%s, err=%d)\n", auth_names[i], err);
        sleep_ms(500);
    }

    return false;
}

static void mdns_service_txt(struct mdns_service *service, void *txt_userdata) {
    (void)txt_userdata;
    mdns_resp_add_service_txtitem(service, "path=/", 6);
    mdns_resp_add_service_txtitem(service, "board=fitpico", 13);
}

static void mdns_init_service(void) {
    mdns_resp_init();
    if (!netif_default) {
        printf("[mDNS] netif 없음\n");
        return;
    }

    err_t err = mdns_resp_add_netif(netif_default, DASHBOARD_HOSTNAME);
    if (err != ERR_OK) {
        printf("[mDNS] netif 등록 실패: %d\n", err);
        return;
    }

    err = mdns_resp_add_service(netif_default, "FitPico Dashboard", "_http",
                                DNSSD_PROTO_TCP, HTTP_PORT, mdns_service_txt, NULL);
    if (err < 0) {
        printf("[mDNS] 서비스 등록 실패: %d\n", err);
        return;
    }

    mdns_resp_announce(netif_default);
    printf("[mDNS] 브라우저에서 열기: http://%s.local/\n", DASHBOARD_HOSTNAME);
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("=== FitPico Web Dashboard ===\n");
    printf("MQTT 브로커: %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);

    if (cyw43_arch_init()) {
        printf("[WiFi] 초기화 실패\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();
    printf("[WiFi] %s 연결 중...\n", WIFI_SSID);
    if (!connect_wifi_with_fallbacks()) {
        printf("[WiFi] 연결 실패\n");
        return 1;
    }

    printf("[WiFi] 연결 완료\n");
    g_wifi_ready = true;
    if (netif_default) {
        printf("[HTTP] 브라우저에서 열기: http://%s/\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
    }

    users_flash_load();
    mqtt_connect_broker();
    if (!http_server_init()) {
        cyw43_arch_deinit();
        return 1;
    }
    mdns_init_service();

    while (true) {
        cyw43_arch_poll();
        maintain_mqtt_connection(now_ms());
        sleep_ms(20);
    }

    cyw43_arch_deinit();
    return 0;
}
