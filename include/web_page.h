#pragma once

// Single-page UI served at "/". Talks to the JSON API in main.cpp.
static const char WEB_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Meshtastic Bridge BLE-TCP</title>
<link rel="icon" href='data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48"><rect x="2" y="2" width="44" height="44" rx="12" fill="%2367ea94"/><g fill="none" stroke="%230d2617" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><circle cx="24" cy="24" r="3"/><circle cx="24" cy="10" r="2.5"/><circle cx="36" cy="17" r="2.5"/><circle cx="36" cy="31" r="2.5"/><circle cx="24" cy="38" r="2.5"/><circle cx="12" cy="31" r="2.5"/><circle cx="12" cy="17" r="2.5"/><path d="M24 21v-8.5M26.6 22.5l7.2-4.2M26.6 25.5l7.2 4.2M24 27v8.5M21.4 25.5l-7.2 4.2M21.4 22.5l-7.2-4.2" stroke-width="2.5"/></g></svg>'>
<style>
/* Meshtastic app palette: navy surfaces, Meshtastic green accent */
:root{--bg:#1c1d29;--card:#2a2c3a;--card2:#363849;--line:#3d4052;--fg:#e4e4ee;--mut:#9fa1b5;
--ok:#67ea94;--warn:#f2b23e;--bad:#f4a093;--acc:#67ea94;--accfg:#0d2617}
*{box-sizing:border-box}body{margin:0;font:15px Roboto,system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:1100px;margin:auto;padding:16px}h1{font-size:20px;margin:0;font-weight:500}
.top{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:12px;margin:4px 0 16px}
nav{display:flex;gap:8px;flex-wrap:wrap}nav .tab.active{background:var(--acc);color:var(--accfg);border-color:var(--acc)}
.grid{display:grid;grid-template-columns:1fr;gap:14px}.col{display:flex;flex-direction:column;gap:14px;min-width:0}
.grid section{margin:0}
@media(min-width:900px){.grid{grid-template-columns:1fr 1fr;align-items:start}
 .grid.home{align-items:stretch}
 .grid.home .col:last-child{display:flex}
 .grid.home section.logs{flex:1;position:relative;min-height:300px}
 .grid.home .logs pre{position:absolute;top:44px;left:16px;right:16px;bottom:16px;height:auto;max-height:none}}
@media(max-width:600px){nav{width:100%;gap:6px}nav button{flex:1;padding:8px 10px;font-size:14px}}
pre{white-space:pre-wrap;word-break:break-word}
.logo{display:flex;align-items:center;gap:10px}.logo svg{flex:none}
.sub{color:var(--mut);font-size:13px}
section{background:var(--card);border-radius:16px;padding:16px;margin-bottom:14px}
.view[hidden]{display:none}
.link{color:var(--acc);cursor:pointer;text-decoration:underline}
h2{font-size:13px;margin:0 0 12px;color:var(--acc);text-transform:uppercase;letter-spacing:.06em;font-weight:600}
.row{display:flex;justify-content:space-between;gap:12px;padding:6px 0;border-bottom:1px solid var(--line)}.row:last-child{border:0}
.row span:first-child{color:var(--mut)}.row span:last-child{text-align:right}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}
label{font-size:13px;color:var(--mut)}
input,button{font:inherit;color:var(--fg)}
input{width:100%;margin:4px 0 10px;padding:11px 12px;border-radius:8px;border:1px solid var(--line);background:transparent}
input:focus{outline:none;border-color:var(--acc)}
select{width:100%;margin:4px 0 0;padding:11px 12px;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--fg);font:inherit}
.logs pre{height:320px}
button{white-space:nowrap;border:0;border-radius:24px;padding:10px 20px;background:var(--acc);color:var(--accfg);font-weight:600;cursor:pointer}
button.sec{background:transparent;border:1px solid var(--line);color:var(--fg)}button:disabled{opacity:.5}
button.restart{background:transparent;border:1px solid var(--warn);color:var(--warn)}
button.restart:hover{background:var(--warn);color:#2a1d00}
button.danger{background:transparent;border:1px solid var(--bad);color:var(--bad)}
.list{margin:8px 0}.item{display:flex;flex-direction:column;padding:10px 12px;border-radius:12px;cursor:pointer;background:var(--card2);margin-bottom:6px}
.item:hover{outline:1px solid var(--line)}.item small{color:var(--mut)}.item.sel{outline:2px solid var(--acc)}
pre{background:#14151e;padding:10px;border-radius:8px;max-height:260px;overflow:auto;font-size:12px;margin:0;color:var(--mut)}
.btns{display:flex;gap:8px;flex-wrap:wrap}.msg{font-size:13px;margin-top:6px;color:var(--mut)}
progress{width:100%;accent-color:var(--acc)}
.mini{background:var(--card2);border-radius:12px;padding:12px;margin-bottom:10px}.mini:last-child{margin-bottom:0}
.mini .hd{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:6px}.mini .t{font-weight:600}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;background:var(--mut)}
.dot.ok{background:var(--ok)}.dot.warn{background:var(--warn)}.dot.bad{background:var(--bad)}
.bars{display:inline-flex;gap:2px;align-items:flex-end;margin-left:6px}.bars i{width:3px;background:var(--ok);border-radius:1px}.bars i.off{background:var(--line)}
.kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;font-size:13px}.kv>span:nth-child(odd){color:var(--mut)}.kv>span:nth-child(even){text-align:right;word-break:break-word}
.sub{color:var(--mut);font-size:12px;font-weight:400}
.by{white-space:nowrap}.by b{color:var(--acc);font-weight:600}
.chk{display:flex;align-items:center;gap:8px;margin:0 0 12px;color:var(--fg);font-size:14px}
.chk input{width:18px;height:18px;margin:0;accent-color:var(--acc)}
.two{display:grid;grid-template-columns:1fr 1fr;gap:10px}
h1{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
h1 .pill{font-size:12px;font-weight:600;background:var(--acc);color:var(--accfg);border-radius:10px;padding:3px 9px}
.brand{display:flex;align-items:center;gap:12px}.brand .logo{width:40px;height:40px;flex:none}
footer{text-align:center;color:var(--mut);font-size:12px;padding:8px 0 24px}
</style></head><body><main>
<div class="top"><div class="brand"><svg class="logo" aria-hidden="true" viewBox="0 0 48 48"><rect x="2" y="2" width="44" height="44" rx="12" fill="#67ea94"/><g fill="none" stroke="#0d2617" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><circle cx="24" cy="24" r="3"/><circle cx="24" cy="10" r="2.5"/><circle cx="36" cy="17" r="2.5"/><circle cx="36" cy="31" r="2.5"/><circle cx="24" cy="38" r="2.5"/><circle cx="12" cy="31" r="2.5"/><circle cx="12" cy="17" r="2.5"/><path d="M24 21v-8.5M26.6 22.5l7.2-4.2M26.6 25.5l7.2 4.2M24 27v8.5M21.4 25.5l-7.2 4.2M21.4 22.5l-7.2-4.2" stroke-width="2.5"/></g></svg><div><h1>Meshtastic Bridge <span class="pill">BLE-TCP</span></h1><div class="sub"><span class="by"><span data-i18n="by"></span> <b>joaopedros2</b></span> · <span id="ver"></span></div></div></div>
<nav><button class="sec tab" id="tab-home" onclick="go('home')" data-i18n="nav_home"></button>
<button class="sec tab" id="tab-settings" onclick="go('settings')" data-i18n="nav_settings"></button>
<button class="restart" onclick="reboot()" data-i18n="reboot"></button></nav></div>
<div id="rebootMsg" class="msg"></div>

<!-- Home: status, Bluetooth node, logs -->
<div class="view" id="view-home">
<section id="setup" hidden style="border:1px solid var(--warn)"><h2 data-i18n="setup_title"></h2>
<div id="setupSteps"></div></section>
<div class="grid home">
<div class="col">
<section><h2 data-i18n="status"></h2><div id="status" data-i18n="loading"></div></section>
</div>
<div class="col">
<section class="logs"><h2>Logs</h2><pre id="logs"></pre></section>
</div>
</div>
</div>

<!-- Settings: WiFi, device, security, firmware, reset -->
<div class="view" id="view-settings" hidden>
<div class="grid">
<div class="col">
<section><h2 data-i18n="ble_node"></h2>
<div class="btns"><button id="bleScanBtn" class="sec" onclick="bleScan()" data-i18n="scan_nodes"></button></div>
<div id="bleList" class="list"></div>
<label data-i18n="mac_address"></label><input id="bleMac" placeholder="c2:23:3f:7d:89:03">
<input id="bleType" type="hidden" value="1">
<label data-i18n="ble_pin"></label><input id="blePin" inputmode="numeric" placeholder="123456">
<button onclick="bleSave()" data-i18n="save_connect"></button><div id="bleMsg" class="msg"></div>
</section>
<section><h2>Wi-Fi</h2>
<div class="btns"><button id="wifiScanBtn" class="sec" onclick="wifiScan()" data-i18n="scan_networks"></button></div>
<div id="wifiList" class="list"></div>
<label>SSID</label><input id="ssid">
<label data-i18n="password"></label><input id="wpass" type="password">
<button onclick="wifiSave()" data-i18n="save_restart"></button><div id="wifiMsg" class="msg"></div>
</section>
<section><h2>Firmware</h2>
<input id="fw" type="file" accept=".bin">
<progress id="fwProg" value="0" max="100" hidden></progress>
<div class="btns"><button onclick="otaUpload()" data-i18n="update"></button></div><div id="fwMsg" class="msg"></div>
</section>
<section><h2 data-i18n="reset"></h2>
<div class="msg" style="margin:0 0 8px" data-i18n="reset_hint"></div>
<button class="danger" onclick="factoryReset()" data-i18n="factory_reset"></button><div id="resetMsg" class="msg"></div>
</section>
</div>
<div class="col">
<section><h2 data-i18n="device"></h2>
<label data-i18n="hostname_label"></label>
<input id="hostname" placeholder="meshtastic-bridge-ble-tcp">
<button onclick="hostSave()" data-i18n="save_restart"></button><div id="hostMsg" class="msg"></div>
<label data-i18n="language" style="display:block;margin-top:14px"></label>
<select id="langSel" onchange="setLang(this.value)"><option value="pt">Português (PT)</option><option value="en">English</option></select>
</section>
<section><h2>MQTT</h2>
<div class="msg" style="margin:0 0 10px" data-i18n="mqtt_hint"></div>
<label class="chk"><input type="checkbox" id="mqEn"><span data-i18n="mqtt_enable"></span></label>
<label data-i18n="mqtt_host"></label><input id="mqHost" placeholder="192.168.1.10">
<div class="two"><div><label data-i18n="mqtt_port"></label><input id="mqPort" inputmode="numeric" placeholder="1883"></div>
<div><label data-i18n="mqtt_interval"></label><input id="mqInt" inputmode="numeric" placeholder="30"></div></div>
<label data-i18n="username"></label><input id="mqUser" autocomplete="off">
<label data-i18n="password"></label><input id="mqPass" type="password" autocomplete="new-password">
<div class="two"><div><label data-i18n="mqtt_discovery"></label><input id="mqDisc" placeholder="homeassistant"></div>
<div><label data-i18n="mqtt_base"></label><input id="mqBase" placeholder="meshtastic-bridge-ble-tcp"></div></div>
<div id="mqTopic" class="msg" style="margin:0 0 10px"></div>
<button onclick="mqttSave()" data-i18n="save"></button><div id="mqMsg" class="msg"></div>
</section>
<section><h2 data-i18n="security"></h2>
<div class="msg" style="margin:0 0 8px" data-i18n="security_hint"></div>
<label data-i18n="username"></label><input id="webUser" placeholder="admin" autocomplete="username">
<label data-i18n="password_empty"></label><input id="webPass" type="password" autocomplete="new-password">
<button onclick="webAuthSave()" data-i18n="save"></button><div id="secMsg" class="msg"></div>
</section>
</div>
</div>
</div>

</main>
<script>
// ---------- i18n (EN / PT-PT) ----------
const I18N={
en:{reboot:'Restart',secured:'secured',nav_home:'Home',nav_settings:'Settings',language:'Page language',mqtt_hint:'Publishes the bridge status to Home Assistant via MQTT discovery (device appears automatically).',mqtt_enable:'Enable MQTT',mqtt_host:'Broker address',mqtt_port:'Port',mqtt_interval:'Update interval (s)',mqtt_discovery:'Discovery prefix',mqtt_base:'Base topic',mqtt_topic:'Topics',pass_unchanged:'(unchanged)',saved_mqtt:'Saved. Connecting to the broker…',err_empty_host:'broker address is empty',err_invalid_port:'invalid port',err_invalid_interval:'interval must be 5–3600 s',setup_title:'First-time setup',status:'Status',loading:'Loading…',ble_node:'Bluetooth node',
 scan_nodes:'Scan nodes',mac_address:'MAC address',ble_pin:'Bluetooth PIN',save_connect:'Save and connect',
 scan_networks:'Scan networks',password:'Password',save_restart:'Save and restart',device:'Device',
 hostname_label:'Network name (router and mDNS: <name>.local)',security:'Security',
 security_hint:'Access to this page. With no password, the page is open.',username:'Username',
 password_empty:'Password (empty = no password)',save:'Save',update:'Update',reset:'Reset',
 reset_hint:'Erases WiFi, Bluetooth node, pairing and password. The ESP32 returns to first-time setup mode.',
 factory_reset:'Factory reset',developed_by:'Developed by',by:'by',
 ble_ready:'Connected',ble_connecting:'Connecting…',ble_failed:'Failed',ble_idle:'Waiting',ble_not_configured:'No node configured',
 node:'Node',network:'Network',system:'System',ble_rssi:'Bluetooth RSSI',last_packet:'Last packet',ble_uptime:'Connected for',ago:s=>s+' s ago',error:'Error',retry:'Next attempt',in_s:s=>'in '+s+' s',
 disconnected:'disconnected',access_point:'Access point',tcp_clients:'TCP clients',none:'none',uptime:'Uptime',
 free_mem:'Free memory',version:'Version',connection:'Connection',no_response:'no response',
 step_wifi:'In <span class="link" onclick="go(\'settings\')">Settings → Wi-Fi</span>, scan and pick your network (2.4 GHz), then save.',
 step_ble:'In <span class="link" onclick="go(\'settings\')">Settings → Bluetooth node</span>, scan for your Meshtastic node, pick it, enter the PIN and save.',
 scanning_ble:'Scanning (5 s)…',no_name:'(no name)',no_nodes:'No Meshtastic node found. Make sure nothing else is connected to it over Bluetooth.',
 saved_connecting:'Saved. Connecting to the node…',scanning:'Scanning…',no_networks:'No networks found',
 confirm_wifi:'The ESP32 will restart and join the new network. Continue?',
 saved_wifi:ap=>'Saved. Restarting… If it fails to connect, join the "'+ap+'" network again.',
 saved_login:'Saved. The browser will ask for the new login.',saved_open:'Saved. Page has no password.',
 pick_file:'Choose the firmware.bin file',updated:'Updated. Restarting…',failed:'Failed: ',net_error:'Network error',
 confirm_reboot:'Restart the ESP32?',rebooting:'Restarting… the page will respond again in ~15 s.',
 confirm_reset:'Erase ALL settings and return to first-time setup?',
 reset_done:ap=>'Restarting. Join the "'+ap+'" WiFi network (password: meshtastic) to set it up.',
 confirm_host:'The ESP32 will restart with the new name. Continue?',saved_restarting:'Saved. Restarting…',
 err_prefix:'Error: ',err_invalid_mac:'invalid MAC address',err_empty_ssid:'SSID is empty',
 err_invalid_user:'username cannot contain ":"',err_invalid_hostname:'invalid name (a-z, 0-9, "-", max 32)'},
pt:{reboot:'Reiniciar',secured:'protegida',nav_home:'Início',nav_settings:'Definições',language:'Idioma da página',mqtt_hint:'Publica o estado da ponte no Home Assistant por MQTT discovery (o dispositivo aparece sozinho).',mqtt_enable:'Ativar MQTT',mqtt_host:'Endereço do broker',mqtt_port:'Porta',mqtt_interval:'Intervalo de envio (s)',mqtt_discovery:'Prefixo de discovery',mqtt_base:'Tópico base',mqtt_topic:'Tópicos',pass_unchanged:'(mantém a atual)',saved_mqtt:'Guardado. A ligar ao broker…',err_empty_host:'endereço do broker vazio',err_invalid_port:'porta inválida',err_invalid_interval:'o intervalo tem de ser entre 5 e 3600 s',setup_title:'Primeira configuração',status:'Estado',loading:'A carregar…',ble_node:'Nó Bluetooth',
 scan_nodes:'Procurar nós',mac_address:'Endereço MAC',ble_pin:'PIN Bluetooth',save_connect:'Guardar e ligar',
 scan_networks:'Procurar redes',password:'Password',save_restart:'Guardar e reiniciar',device:'Dispositivo',
 hostname_label:'Nome na rede (router e mDNS: <nome>.local)',security:'Segurança',
 security_hint:'Acesso a esta página. Sem password, a página fica aberta.',username:'Utilizador',
 password_empty:'Password (vazio = sem password)',save:'Guardar',update:'Atualizar',reset:'Repor',
 reset_hint:'Apaga Wi-Fi, nó Bluetooth, emparelhamento e password. O ESP32 volta ao modo de primeira configuração.',
 factory_reset:'Repor definições de fábrica',developed_by:'Desenvolvido por',by:'por',
 ble_ready:'Ligado',ble_connecting:'A ligar…',ble_failed:'Falhou',ble_idle:'Em espera',ble_not_configured:'Sem nó configurado',
 node:'Nó',network:'Rede',system:'Sistema',ble_rssi:'RSSI Bluetooth',last_packet:'Último pacote',ble_uptime:'Ligado há',ago:s=>'há '+s+' s',error:'Erro',retry:'Nova tentativa',in_s:s=>'em '+s+' s',
 disconnected:'desligado',access_point:'Ponto de acesso',tcp_clients:'Clientes TCP',none:'nenhum',uptime:'Uptime',
 free_mem:'Memória livre',version:'Versão',connection:'Ligação',no_response:'sem resposta',
 step_wifi:'Em <span class="link" onclick="go(\'settings\')">Definições → Wi-Fi</span>, procura e escolhe a tua rede (2,4 GHz) e guarda.',
 step_ble:'Em <span class="link" onclick="go(\'settings\')">Definições → Nó Bluetooth</span>, procura o teu nó Meshtastic, escolhe-o, indica o PIN e guarda.',
 scanning_ble:'A procurar (5 s)…',no_name:'(sem nome)',no_nodes:'Nenhum nó Meshtastic encontrado. Confirma que nada está ligado a ele por Bluetooth.',
 saved_connecting:'Guardado. A ligar ao nó…',scanning:'A procurar…',no_networks:'Nenhuma rede encontrada',
 confirm_wifi:'O ESP32 vai reiniciar e ligar-se à nova rede. Continuar?',
 saved_wifi:ap=>'Guardado. A reiniciar… Se a ligação falhar, volta a ligar-te à rede "'+ap+'".',
 saved_login:'Guardado. O browser vai pedir o novo login.',saved_open:'Guardado. Página sem password.',
 pick_file:'Escolhe o ficheiro firmware.bin',updated:'Atualizado. A reiniciar…',failed:'Falhou: ',net_error:'Erro de rede',
 confirm_reboot:'Reiniciar o ESP32?',rebooting:'A reiniciar… a página volta a responder em ~15 s.',
 confirm_reset:'Apagar TODAS as definições e voltar à primeira configuração?',
 reset_done:ap=>'A reiniciar. Liga-te à rede Wi-Fi "'+ap+'" (password: meshtastic) para configurar.',
 confirm_host:'O ESP32 vai reiniciar com o novo nome. Continuar?',saved_restarting:'Guardado. A reiniciar…',
 err_prefix:'Erro: ',err_invalid_mac:'endereço MAC inválido',err_empty_ssid:'SSID vazio',
 err_invalid_user:'o utilizador não pode conter ":"',err_invalid_hostname:'nome inválido (a-z, 0-9, "-", máx. 32)'}
};
function savedLang(){try{return localStorage.getItem('lang')}catch(e){return null}}
let lang=savedLang()||((navigator.language||'').toLowerCase().startsWith('pt')?'pt':'en');
const t=(k,...a)=>{const v=I18N[lang][k];return typeof v==='function'?v(...a):(v??k)};
const errText=e=>t('err_prefix')+(I18N[lang]['err_'+e]||e);
function applyLang(){
  document.documentElement.lang=lang==='pt'?'pt-PT':'en';
  document.querySelectorAll('[data-i18n]').forEach(el=>el.textContent=t(el.dataset.i18n));
  $('langSel').value=lang;
}
function setLang(l){lang=l;try{localStorage.setItem('lang',lang)}catch(e){}applyLang();refresh()}

// ---------- page logic ----------
const $=id=>document.getElementById(id);
const post=(url,data)=>fetch(url,{method:'POST',body:new URLSearchParams(data||{})}).then(r=>r.json());
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const bleClass={ready:'ok',connecting:'warn',failed:'bad',idle:'warn',not_configured:'bad'};
let formFilled=false,apName='';

function row(k,v,cls){return `<div class="row"><span>${k}</span><span class="${cls||''}">${v}</span></div>`}
// Device stamps lines with uptime (H:MM:SS); show them in the browser's local time.
// Same idea as ESPHome: the viewer supplies the clock, so no NTP is needed.
function logTime(line,uptimeNow){
  const m=line.match(/^(\d+):(\d\d):(\d\d) (.*)$/);
  if(!m||!uptimeNow)return line;
  const lineUp=+m[1]*3600+(+m[2])*60+(+m[3]);
  const d=new Date(Date.now()-(uptimeNow-lineUp)*1000);
  const loc=lang==='pt'?'pt-PT':'en-GB';
  let ts=d.toLocaleTimeString(loc,{hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false});
  if(d.toDateString()!==new Date().toDateString())ts=d.toLocaleDateString(loc,{day:'2-digit',month:'2-digit'})+' '+ts;
  return ts+' '+m[4];
}
// Signal bars: 4 steps of 10 dB below `best` (BLE best≈-70, Wi-Fi best≈-60)
function signal(rssi,best){
  const n=rssi>=best?4:rssi>=best-10?3:rssi>=best-20?2:rssi>=best-30?1:0;
  const bars=[4,7,10,13].map((ht,i)=>`<i style="height:${ht}px" class="${i<n?'':'off'}"></i>`).join('');
  return `<span class="sub">${rssi} dBm<span class="bars">${bars}</span></span>`;
}
function mini(title,cls,state,right,rows){
  const dot=cls?`<span class="dot ${cls}"></span>`:'';
  const st=state?` <span class="${cls} sub">· ${state}</span>`:'';
  return `<div class="mini"><div class="hd"><span class="t">${dot}${title}${st}</span>${right}</div>
    <div class="kv">${rows.map(([k,v])=>`<span>${k}</span><span>${v}</span>`).join('')}</div></div>`;
}
// Short durations keep seconds (useful right after a reconnect)
function fmtDur(s){return s<60?s+' s':s<3600?Math.floor(s/60)+'m '+(s%60)+'s':fmtUp(s)}
function fmtUp(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'d ':'')+h+'h '+m+'m'}

async function refresh(){
  try{
    const s=await (await fetch('/api/status')).json();
    // Option C: one mini-card per area (Bluetooth, Wi-Fi, System)
    const b=s.ble,w=s.wifi,cl=s.tcp.clients;
    let bk=[[t('node'),esc(b.name||'—')],['MAC',esc(b.target||'—')]];
    if(b.state==='ready')bk.push([t('ble_uptime'),fmtDur(b.connected_s)],[t('last_packet'),b.last_rx_s<0?'—':t('ago',b.last_rx_s)]);
    if(b.state==='failed')bk.push([t('error'),`<span class="bad">${esc(b.stage)} (${b.error})</span>`],[t('retry'),t('in_s',Math.ceil(b.retry_in_ms/1000))]);
    let wk=[[t('network'),w.connected?esc(w.ssid):`<span class="bad">${t('disconnected')}</span>`],['IP',w.ip||'—']];
    if(w.ap)wk.push([t('access_point'),`<span class="warn">${esc(w.ap_ssid)} · ${w.ap_ip}</span>`]);
    wk.push([t('tcp_clients')+' ('+cl.length+'/'+s.tcp.max+')',cl.length?cl.join('<br>'):t('none')]);
    if(s.mqtt.enabled)wk.push(['MQTT',s.mqtt.connected?`<span class="ok">${t('ble_ready')}</span>`:`<span class="bad">${t('disconnected')} (rc ${s.mqtt.rc})</span>`]);
    let h=mini('Bluetooth',bleClass[b.state],t('ble_'+b.state),b.state==='ready'?signal(b.rssi,-70):'',bk);
    h+=mini('Wi-Fi',w.connected?'ok':'bad',w.connected?t('ble_ready'):t('disconnected'),w.connected?signal(w.rssi,-60):'',wk);
    h+=mini(t('system'),'','',`<span class="sub">v${esc(s.version)}</span>`,[[t('uptime'),fmtUp(s.uptime)],[t('free_mem'),Math.round(s.heap/1024)+' KB']]);
    $('status').innerHTML=h;
    $('ver').textContent='v'+s.version;
    const steps=[];
    if(!s.wifi.configured)steps.push(t('step_wifi'));
    if(!s.ble.target)steps.push(t('step_ble'));
    $('setup').hidden=!steps.length;$('setupSteps').innerHTML=steps.map((x,i)=>(i+1)+'. '+x).join('<br>');
    apName=s.wifi.ap_ssid;
    if(!formFilled){$('bleMac').value=s.ble.target;$('bleType').value=s.ble.addr_type;$('ssid').value=s.wifi.ssid;$('hostname').value=s.hostname;$('webUser').value=s.web_user;
      const m=s.mqtt;$('mqEn').checked=m.enabled;$('mqHost').value=m.host;$('mqPort').value=m.port;$('mqUser').value=m.user;
      $('mqDisc').value=m.discovery;$('mqBase').value=m.base;$('mqInt').value=m.interval;$('mqPass').placeholder=m.has_pass?t('pass_unchanged'):'';
      formFilled=true}
    $('mqTopic').textContent=t('mqtt_topic')+': '+s.mqtt.topic+'/…';
    const lr=await fetch('/api/logs'),up=+lr.headers.get('X-Uptime'),txt=await lr.text();
    const pre=$('logs'),atEnd=pre.scrollHeight-pre.scrollTop-pre.clientHeight<20;
    pre.textContent=txt.split('\n').filter(Boolean).map(l=>logTime(l,up)).join('\n');
    if(atEnd)pre.scrollTop=pre.scrollHeight;  // follow new lines, like tail -f
  }catch(e){$('status').innerHTML=row(t('connection'),t('no_response'),'bad')}
}

async function bleScan(){
  $('bleScanBtn').disabled=true;$('bleList').textContent=t('scanning_ble');
  await post('/api/ble/scan');
  for(let i=0;i<20;i++){
    await new Promise(r=>setTimeout(r,500));
    const r=await (await fetch('/api/ble/scan')).json();
    if(r.scanning)continue;
    $('bleList').innerHTML=r.devices.length?r.devices.map(d=>
      `<div class="item" onclick="pickBle(this,'${d.addr}',${d.type})"><b>${esc(d.name||t('no_name'))}</b><small>${d.addr} · ${d.rssi} dBm</small></div>`).join('')
      :esc(t('no_nodes'));
    break;
  }
  $('bleScanBtn').disabled=false;
}
function pickBle(el,addr,type){document.querySelectorAll('#bleList .item').forEach(i=>i.classList.remove('sel'));el.classList.add('sel');$('bleMac').value=addr;$('bleType').value=type}
async function bleSave(){
  const r=await post('/api/ble/config',{mac:$('bleMac').value.trim(),type:$('bleType').value,pin:$('blePin').value.trim()});
  $('bleMsg').textContent=r.ok?t('saved_connecting'):errText(r.error);
}

async function wifiScan(){
  $('wifiScanBtn').disabled=true;$('wifiList').textContent=t('scanning');
  await post('/api/wifi/scan');
  for(let i=0;i<30;i++){
    await new Promise(r=>setTimeout(r,700));
    const r=await (await fetch('/api/wifi/scan')).json();
    if(r.scanning)continue;
    $('wifiList').innerHTML=r.networks.map(n=>
      `<div class="item" onclick="$('ssid').value=this.dataset.s" data-s="${esc(n.ssid)}"><b>${esc(n.ssid)}</b><small>${n.rssi} dBm${n.secure?' · '+t('secured'):''}</small></div>`).join('')||esc(t('no_networks'));
    break;
  }
  $('wifiScanBtn').disabled=false;
}
async function wifiSave(){
  if(!confirm(t('confirm_wifi')))return;
  const r=await post('/api/wifi/config',{ssid:$('ssid').value,pass:$('wpass').value});
  $('wifiMsg').textContent=r.ok?t('saved_wifi',apName):errText(r.error);
}
async function webAuthSave(){
  const r=await post('/api/web/auth',{user:$('webUser').value,pass:$('webPass').value});
  $('secMsg').textContent=r.ok?($('webPass').value?t('saved_login'):t('saved_open')):errText(r.error);
}
function otaUpload(){
  const f=$('fw').files[0];if(!f){$('fwMsg').textContent=t('pick_file');return}
  const fd=new FormData();fd.append('firmware',f);
  const x=new XMLHttpRequest();x.open('POST','/api/update');
  $('fwProg').hidden=false;
  x.upload.onprogress=e=>{$('fwProg').value=e.loaded/e.total*100};
  x.onload=()=>{let ok=false;try{ok=x.status==200&&JSON.parse(x.responseText).ok}catch(e){}
    $('fwMsg').textContent=ok?t('updated'):t('failed')+x.responseText};
  x.onerror=()=>{$('fwMsg').textContent=t('net_error')};
  x.send(fd);
}
async function reboot(){if(confirm(t('confirm_reboot'))){await post('/api/reboot');$('rebootMsg').textContent=t('rebooting')}}
async function factoryReset(){
  if(!confirm(t('confirm_reset')))return;
  const r=await post('/api/factory-reset');
  $('resetMsg').textContent=r.ok?t('reset_done',apName):t('net_error');
}
async function mqttSave(){
  const r=await post('/api/mqtt/config',{enabled:$('mqEn').checked?'1':'0',host:$('mqHost').value,port:$('mqPort').value||'1883',
    user:$('mqUser').value,pass:$('mqPass').value,discovery:$('mqDisc').value,base:$('mqBase').value,interval:$('mqInt').value||'30'});
  $('mqMsg').textContent=r.ok?t('saved_mqtt'):errText(r.error);
  if(r.ok)$('mqPass').value='';
}
async function hostSave(){
  if(!confirm(t('confirm_host')))return;
  const r=await post('/api/device/config',{hostname:$('hostname').value});
  $('hostMsg').textContent=r.ok?t('saved_restarting'):errText(r.error);
}

function go(view){location.hash=view==='home'?'':view;showView()}
function showView(){
  const v=location.hash==='#settings'?'settings':'home';
  for(const n of ['home','settings']){$('view-'+n).hidden=n!==v;$('tab-'+n).classList.toggle('active',n===v)}
}
window.addEventListener('hashchange',showView);

applyLang();showView();refresh();setInterval(refresh,3000);
</script></body></html>)HTML";
