#include "https_setup_server.h"

#include "https_credentials.h"
#include "network_services.h"
#include "settings.h"
#include "task_diagnostics.h"
#include "time_services.h"
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif
#include <string.h>

extern const char* FW_VERSION;
extern const char* FW_NAME;
extern String gpsSyncReturnUrl;
extern void sendNoCacheHeaders();
extern String urlDecodeSimple(String s);
extern String currentLocationSource;

uint16_t httpsSetupPort() {
#if defined(ESP32)
  return HTTPS_SETUP_PORT;
#else
  return 0;
#endif
}

bool httpsWebAutoStartEnabled() {
#if defined(ESP32)
  return HTTPS_WEB_AUTO_START;
#else
  return false;
#endif
}

void handleHttpsSetupRedirectPage() {
  String target = String("https://") + WiFi.softAPIP().toString() + "/";
  if (WiFi.status() == WL_CONNECTED && !requestCameFromApSubnet()) {
    target = String("https://") + WiFi.localIP().toString() + "/";
  }
  target += "?return=" + urlEncodeSimple(cleanGpsReturnUrl(gpsSyncReturnUrl));
  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");
  server.sendHeader("Location", target);
  server.send(302, "text/plain", "Redirecting to HTTPS GPS Sync");
}

String urlEncodeSimple(const String &s) {
  String out;
  out.reserve(s.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

String cleanGpsReturnUrl(const String &raw) {
  String r = raw;
  r.trim();
  if (!r.startsWith("http://")) return String("http://") + WiFi.softAPIP().toString() + "/";
  int scheme = r.indexOf("://");
  int start = (scheme >= 0) ? scheme + 3 : 0;
  int slash = r.indexOf('/', start);
  if (slash < 0) return r + "/";
  String base = r.substring(0, slash + 1);
  return base;
}

String gpsReturnUrlWithCacheBuster() {
  String r = cleanGpsReturnUrl(gpsSyncReturnUrl);
  r += "?gpssync_done=";
  r += String(millis());
  return r;
}

void scheduleHttpsSetupStop(unsigned long delayMs) {
  if (httpsSetupServerHandle) {
    httpsSetupStopAtMillis = millis() + delayMs;
  }
}

void serviceHttpsSetupStop() {
  if (httpsSetupServerHandle && httpsSetupStopAtMillis != 0 && (long)(millis() - httpsSetupStopAtMillis) >= 0) {
    httpd_handle_t h = httpsSetupServerHandle;
    httpsSetupServerHandle = NULL;
    httpsSetupStopAtMillis = 0;
    Serial.println("[HTTPS] stopping GPS Sync HTTPS server after completed sync");
    httpd_ssl_stop(h);
  }
}

void handleStartHttpsPage() {
#if defined(ESP32)
  if (server.hasArg("return")) {
    String r = urlDecodeSimple(server.arg("return"));
    if (r.startsWith("http://")) gpsSyncReturnUrl = cleanGpsReturnUrl(r);
  }

  startHttpsSetupServer();

  String target = String("https://") + WiFi.softAPIP().toString() + "/";
  if (WiFi.status() == WL_CONNECTED && !requestCameFromApSubnet()) {
    target = String("https://") + WiFi.localIP().toString() + "/";
  }
  target += "?return=" + urlEncodeSimple(cleanGpsReturnUrl(gpsSyncReturnUrl));

  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");

  if (server.hasArg("json")) {
    if (httpsSetupServerHandle) {
      String json = "{\"ok\":true,\"url\":\"" + target + "\"}";
      server.send(200, "application/json", json);
    } else {
      server.send(500, "application/json", "{\"ok\":false,\"error\":\"HTTPS GPS Sync server failed to start\"}");
    }
  } else {
    if (httpsSetupServerHandle) server.send(200, "text/plain", target);
    else server.send(500, "text/plain", "HTTPS GPS Sync server failed to start");
  }
#else
  sendNoCacheHeaders();
  server.sendHeader("Connection", "close");
  if (server.hasArg("json")) server.send(501, "application/json", "{\"ok\":false,\"error\":\"HTTPS only available on ESP32\"}");
  else server.send(501, "text/plain", "HTTPS only available on ESP32");
#endif
}

bool requestCameFromApSubnet() {
#if defined(ESP32)
  if (!apRunning) return false;
  IPAddress rip = server.client().remoteIP();
  IPAddress ap = WiFi.softAPIP();
  return rip[0] == ap[0] && rip[1] == ap[1] && rip[2] == ap[2];
#else
  return false;
#endif
}

void redirectToHttpsSetup443() {
#if defined(ESP32)
  String target = String("https://") + WiFi.softAPIP().toString() + "/";
  server.sendHeader("Location", target);
  server.send(302, "text/plain", "Redirecting to secure browser time/location setup");
#else
  sendWebPage();
#endif
}

String httpsAutoDeviceTimeLocationScript() {
  String js;
  js.reserve(2600);
  String ret = cleanGpsReturnUrl(gpsSyncReturnUrl);
  ret.replace("\\", "\\\\");
  ret.replace("'", "\\'");
  js += "<script>";
  js += "function nxEnc(v){return encodeURIComponent(v)};";
  js += "function nxReturnUrl(){try{let p=new URLSearchParams(location.search);return p.get('return')||'";
  js += ret;
  js += "'}catch(e){return '";
  js += ret;
  js += "'}};";
  js += "function nxMsg(s){let m=document.getElementById('httpsMsg');if(m)m.textContent=s};";
  js += "function nxSendDeviceSite(lat,lon,acc){let d=new Date();let off=-d.getTimezoneOffset();";
  js += "let ret=nxReturnUrl();";
  js += "let u='/set_device_site_time?year='+d.getFullYear()+'&month='+(d.getMonth()+1)+'&day='+d.getDate()+'&hour='+d.getHours()+'&minute='+d.getMinutes()+'&second='+d.getSeconds()+'&offset='+off+'&return='+nxEnc(ret);";
  js += "if(lat!==null&&lon!==null){u+='&lat='+nxEnc(lat)+'&lon='+nxEnc(lon);if(acc!==null)u+='&elev=0'}";
  js += "fetch(u,{cache:'no-store'}).then(r=>r.text()).then(()=>{nxMsg(lat!==null?'Browser time/date/location saved. Returning to main page in 5 seconds...':'Browser time/date saved; location was not provided. Returning to main page in 5 seconds...');setTimeout(()=>{let sep=ret.includes('?')?'&':'?';location.replace(ret+sep+'gpssync_return='+Date.now())},5000)}).catch(e=>{nxMsg('GPS Sync save failed: '+e)});}";
  js += "function nxTryBrowserSiteTime(){nxMsg('Requesting browser location/time...');";
  js += "if(navigator.geolocation){navigator.geolocation.getCurrentPosition(p=>nxSendDeviceSite(p.coords.latitude,p.coords.longitude,p.coords.accuracy),e=>nxSendDeviceSite(null,null,null),{enableHighAccuracy:true,timeout:10000,maximumAge:300000});}";
  js += "else{nxSendDeviceSite(null,null,null)}};";
  js += "window.addEventListener('load',()=>setTimeout(nxTryBrowserSiteTime,800));";
  js += "</script>";
  return js;
}

String buildHttpsMainPage() {
  String page;
  page.reserve(3200);
  String ret = cleanGpsReturnUrl(gpsSyncReturnUrl);
  ret.replace("\\", "\\\\");
  ret.replace("'", "&#39;");
  page += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<meta http-equiv='Cache-Control' content='no-store'>";
  page += "<title>NexStar GPS Sync</title></head><body style='font-family:Arial,sans-serif;background:#111;color:#eee;margin:18px'>";
  page += "<h2>NexStar GPS Sync</h2>";
  page += "<div id='httpsMsg' style='padding:10px;margin:10px 0;border:1px solid #555;background:#222;border-radius:8px'>Preparing GPS Sync...</div>";
  page += "<p>This HTTPS page lets the browser provide location permission. Browser time/date is also copied to the ESP32.</p>";
  page += "<p>Return target: <span style='color:#8cf'>";
  page += ret;
  page += "</span></p>";
  page += "<p><a style='display:inline-block;padding:10px 14px;background:#2d6cdf;color:#fff;text-decoration:none;border-radius:6px' href='";
  page += cleanGpsReturnUrl(gpsSyncReturnUrl);
  page += "'>Return without syncing</a></p>";
  page += "<p><a style='color:#8cf' href='/timeloc_status'>Time/Location Status</a> | <a style='color:#8cf' href='/status'>HTTPS Status</a></p>";
  page += httpsAutoDeviceTimeLocationScript();
  page += "</body></html>";
  return page;
}

void httpsSendText(httpd_req_t *req, const char *type, const String &body) {
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, body.c_str(), body.length());
}

String jsonBool(bool v) {
  return v ? "true" : "false";
}

esp_err_t httpsCatchAllHandler(httpd_req_t *req) {
  String uri = String(req->uri);
  String ret = httpParamValue(uri, "return");
  if (ret.length() && ret.startsWith("http://")) {
    gpsSyncReturnUrl = cleanGpsReturnUrl(ret);
  }

  String path = uri;
  int q = path.indexOf('?');
  if (q >= 0) path = path.substring(0, q);

  if (path == "/" || path == "/index.html" || path == "/gps_sync") {
    httpsSendText(req, "text/html", buildHttpsMainPage());
    return ESP_OK;
  }

  if (path == "/set_device_site_time") {
    bool gotLocation = false;
    applyHttpsBrowserTimeLocationFromUrl(uri, gotLocation);
    scheduleHttpsSetupStop(1800);
    ret = httpParamValue(uri, "return");
    if (ret.length() && ret.startsWith("http://")) gpsSyncReturnUrl = cleanGpsReturnUrl(ret);
    httpsSendText(req, "text/plain", gotLocation ? "HTTPS browser time/location saved" : "HTTPS browser time saved; location not provided");
    return ESP_OK;
  }

  if (path == "/timeloc_status") {
    String s;
    s.reserve(1200);
    s += "Time / Location status " + String(FW_VERSION) + "\n";
    s += "Time valid: " + String(timeValid ? "true" : "false") + "\n";
    s += "Time source: " + String(timeSourceName(currentTimeSource)) + "\n";
    s += "UTC offset minutes: " + String(utcOffsetMinutes) + "\n";
    s += "UTC offset source: " + utcOffsetSource + "\n";
    s += "Local date: " + String(localYear) + "-" + String(localMonth) + "-" + String(localDay) + "\n";
    s += "Local time: " + String(localHour) + ":" + String(localMinute) + ":" + String(localSecond) + "\n";
    s += "Site valid: " + String(siteValid ? "true" : "false") + "\n";
    s += "Location source: " + currentLocationSource + "\n";
    s += "Latitude: " + String(siteLatitudeDeg, 6) + "\n";
    s += "Longitude: " + String(siteLongitudeDeg, 6) + "\n";
    httpsSendText(req, "text/plain", s);
    return ESP_OK;
  }

  if (path == "/status") {
    String body = "{\"ok\":true,\"https\":true,\"timeValid\":" + jsonBool(timeValid) +
                  ",\"siteValid\":" + jsonBool(siteValid) +
                  ",\"apIP\":\"" + WiFi.softAPIP().toString() + "\"" +
                  ",\"staIP\":\"" + WiFi.localIP().toString() + "\"}";
    httpsSendText(req, "application/json", body);
    return ESP_OK;
  }

  if (path == "/mount_test") {
    String s = "Mount test is available on the normal HTTP web UI.\n";
    httpsSendText(req, "text/plain", s);
    return ESP_OK;
  }

  httpsSendText(req, "text/plain", "HTTPS GPS Sync route not implemented in " + String(FW_VERSION) + ": " + uri);
  return ESP_OK;
}

void startHttpsSetupServer() {
  if (httpsSetupServerHandle) return;

  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.port_secure = HTTPS_SETUP_PORT;
  conf.servercert = (const unsigned char *)HTTPS_SETUP_CERT;
  conf.servercert_len = strlen(HTTPS_SETUP_CERT) + 1;
  conf.prvtkey_pem = (const unsigned char *)HTTPS_SETUP_KEY;
  conf.prvtkey_len = strlen(HTTPS_SETUP_KEY) + 1;
  conf.httpd.uri_match_fn = httpd_uri_match_wildcard;

  esp_err_t ret = httpd_ssl_start(&httpsSetupServerHandle, &conf);
  if (ret != ESP_OK) {
    Serial.printf("HTTPS web UI server failed to start: err=%d\n", (int)ret);
    httpsSetupServerHandle = NULL;
    return;
  }

  httpd_uri_t root_uri = {};
  root_uri.uri = "/";
  root_uri.method = HTTP_GET;
  root_uri.handler = httpsCatchAllHandler;
  root_uri.user_ctx = NULL;
  httpd_register_uri_handler(httpsSetupServerHandle, &root_uri);

  httpd_uri_t set_uri = {};
  set_uri.uri = "/set_device_site_time*";
  set_uri.method = HTTP_GET;
  set_uri.handler = httpsCatchAllHandler;
  set_uri.user_ctx = NULL;
  httpd_register_uri_handler(httpsSetupServerHandle, &set_uri);

  httpd_uri_t any_uri = {};
  any_uri.uri = "/*";
  any_uri.method = HTTP_GET;
  any_uri.handler = httpsCatchAllHandler;
  any_uri.user_ctx = NULL;
  httpd_register_uri_handler(httpsSetupServerHandle, &any_uri);

  Serial.printf("HTTPS web UI ready: https://%s/\n", WiFi.softAPIP().toString().c_str());
}
