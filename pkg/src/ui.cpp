#include "ui.h"
#include "app.h"
#include "api.h"
#include "scene.h"
#include "image.h"
#include "connstate.h"
#include "i18n.h"
#include "log.h"
#include "service.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <string>
#include <vector>

#include <orbis/Pad.h>
#include <orbis/UserService.h>

extern "C" uint64_t sceKernelGetProcessTime(void);

#ifndef APP_VERSION
#define APP_VERSION "0.0.0.0"
#endif

#define FRAME_WIDTH   1920
#define FRAME_HEIGHT  1080
#define FRAME_DEPTH      4

static const int FONT_PX   = 26;
static const int ROW_H     = 40;    // чуть выше, чтобы под строкой поместилась полоска прогресса
static const int BAR_H     = 4;
static const int BAR_GAP   = 4;     // отступ снизу строки до полоски (разделяет соседние строки)
static const int MARGIN_X  = 40;
static const int BANNER_LEFT = 12;   // логотип прижат ближе к левому краю, отдельно от MARGIN_X
static const int BANNER_TOP = 16;     // отступ сверху для баннера/заголовка
static const int BANNER_H   = 64;     // высота баннера на экране (title.png вписывается в неё)
static const int HEADER_Y  = BANNER_TOP + BANNER_H + 14;
static const int ROWS_Y    = HEADER_Y + ROW_H + 6;
static const int FOOTER_Y  = 1000;    // ниже - подсказка и титры, см. HINT_BASELINE/CREDIT_BASELINE
static const int VISIBLE   = (FOOTER_Y - 10 - ROWS_Y) / ROW_H;

// Колонки (в символах моноширинного шрифта)
static const int COL_NUM   = 0,  W_NUM   = 3;
static const int COL_NAME  = 5,  W_NAME  = 46;    // было 56: 10 символов отдано под новую колонку ETA
static const int COL_SIZE  = 53, W_SIZE  = 10;
static const int COL_DONE  = 65, W_DONE  = 17;
static const int COL_ETA   = 84, W_ETA   = 8;      // новая колонка: сколько примерно осталось
static const int COL_SPEED = 94, W_SPEED = 13;     // позиция не изменилась
static const int COL_PEERS = 109, W_PEERS = 6;     // позиция не изменилась

static const Color C_BG    = { 22,  24,  30 };
static const Color C_ROW2  = { 28,  31,  39 };
static const Color C_HEAD  = { 45,  50,  64 };
static const Color C_SEL   = { 72,  82,  96 };    // выделение: серый в тон прежнему синему (оттенок 215)
static const Color C_TEXT  = { 230, 230, 230 };
static const Color C_DIM   = { 150, 155, 165 };
static const Color C_DIMMER = { 95,  99,  108 };   // тусклее C_DIM, для титров внизу
static const Color C_TRACK  = { 55,  59,  68 };    // серая "дорожка" полоски прогресса
static const Color C_FILL   = { 81, 171, 236 };    // голубая заливка полоски (C_GREEN оставлен для статуса complete)
static const Color C_GREEN = { 110, 210, 130 };
static const Color C_AMBER = { 235, 180,  80 };
static const Color C_RED   = { 235, 100, 100 };
static const Color C_PANEL = { 40,  44,  56 };
static const Color C_EDGE  = { 110, 120, 150 };

static Scene2D* g_scene = NULL;
static FT_Face  g_font;
static int      g_cellW = 16;
static bool     g_ready = false;
static bool     g_padOk = false;

static int      g_sel = 0;
static int      g_top = 0;
static int      g_redraw = 2;
static int      g_frameID = 0;
static uint64_t g_lastTick = 0;
static Image    g_banner;
static bool     g_bannerOk = false;
static int      g_bannerW  = 0;   // ширина на экране при высоте BANNER_H (с сохранением пропорций)
static uint64_t g_lastRefresh = 0;

enum Modal { MODAL_NONE, MODAL_DELETE, MODAL_QUIT };
static Modal        g_modal = MODAL_NONE;
static std::string  g_modalHash;
static std::string  g_modalTitle;

// Запуск службы (payload) из приложения: проверка, ожидание, ошибка, готово. См. svc_step() ниже.
enum SvcState { SVC_CHECK, SVC_WAITING, SVC_FAILED, SVC_READY };
static SvcState     g_svc = SVC_CHECK;
static int          g_svcChecks = 0;         // сколько раз подряд служба не ответила при проверке
static uint64_t     g_svcDeadline = 0;       // до какого момента ждём ответа после отправки payload
static std::string  g_svcErr;

// ---------------------------------------------------------------- UTF-8

static int decode_utf8(const unsigned char*& p)
{
    unsigned c = *p++;
    if (c < 0x80) return (int)c;

    int extra;
    unsigned cp;
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return '?';

    while (extra-- > 0) {
        if ((*p & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (*p & 0x3F);
        p++;
    }
    return (int)cp;
}

static int count_cp(const std::string& s)
{
    const unsigned char* p = (const unsigned char*)s.c_str();
    int n = 0;
    while (*p) { decode_utf8(p); n++; }
    return n;
}

static std::string fit(const std::string& s, int maxChars)
{
    if (count_cp(s) <= maxChars) return s;
    const unsigned char* start = (const unsigned char*)s.c_str();
    const unsigned char* p = start;
    for (int i = 0; i < maxChars - 1; i++) decode_utf8(p);
    return std::string((const char*)start, (size_t)(p - start)) + "\xE2\x80\xA6";
}

// ---------------------------------------------------------------- текст

static void draw_text(const std::string& s, int x, int baseline, Color fg, Color bg)
{
    FT_GlyphSlot slot = g_font->glyph;
    const unsigned char* p = (const unsigned char*)s.c_str();

    while (*p) {
        int cp = decode_utf8(p);
        FT_UInt gi = FT_Get_Char_Index(g_font, (FT_ULong)cp);

        if (FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) continue;
        if (FT_Render_Glyph(slot, ft_render_mode_normal)) continue;

        for (unsigned yy = 0; yy < slot->bitmap.rows; yy++) {
            for (unsigned xx = 0; xx < slot->bitmap.width; xx++) {
                unsigned a = slot->bitmap.buffer[yy * slot->bitmap.pitch + xx];
                if (a == 0) continue;

                int px = x + (int)xx + slot->bitmap_left;
                int py = baseline + (int)yy - slot->bitmap_top;
                if (px < 0 || py < 0 || px >= FRAME_WIDTH || py >= FRAME_HEIGHT) continue;

                Color c;
                c.r = (uint8_t)((bg.r * (255 - a) + fg.r * a) / 255);
                c.g = (uint8_t)((bg.g * (255 - a) + fg.g * a) / 255);
                c.b = (uint8_t)((bg.b * (255 - a) + fg.b * a) / 255);
                g_scene->PutPixel(px, py, c);
            }
        }
        x += (int)(slot->advance.x >> 6);
    }
}

// ---------------------------------------------------------------- иконки кнопок геймпада
// Рисуются кодом (без картинок): Cross, Circle, Square, Triangle, Options (три полоски) и стрелки вверх/вниз.
// Токены в тексте: {cross} {circle} {square} {triangle} {options} {updown}.

enum Icon { ICON_CROSS, ICON_CIRCLE, ICON_SQUARE, ICON_TRIANGLE, ICON_OPTIONS, ICON_UPDOWN };
static const int ICON_S = 26;   // сторона иконки в пикселях

static bool in_tri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
{
    float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

// u, v в диапазоне -1..1 (центр иконки в нуле, v растёт вниз).
static bool icon_hit(Icon k, float u, float v)
{
    switch (k) {
    case ICON_CIRCLE: {
        float r = sqrtf(u * u + v * v);
        return r < 0.88f && r > 0.60f;
    }
    case ICON_CROSS: {
        float a = fabsf(u - v) * 0.7071f, b = fabsf(u + v) * 0.7071f;
        return fabsf(u) < 0.82f && fabsf(v) < 0.82f && (a < 0.17f || b < 0.17f);
    }
    case ICON_SQUARE: {
        float m = fmaxf(fabsf(u), fabsf(v));
        return m < 0.80f && m > 0.56f;
    }
    case ICON_TRIANGLE:
        return in_tri(u, v, 0.0f, -0.90f, -0.90f, 0.70f, 0.90f, 0.70f)
            && !in_tri(u, v, 0.0f, -0.42f, -0.495f, 0.46f, 0.495f, 0.46f);
    case ICON_OPTIONS:
        return fabsf(u) < 0.80f && (fabsf(v + 0.55f) < 0.11f || fabsf(v) < 0.11f || fabsf(v - 0.55f) < 0.11f);
    case ICON_UPDOWN:
        return in_tri(u, v, 0.0f, -0.95f, -0.60f, -0.15f, 0.60f, -0.15f)
            || in_tri(u, v, -0.60f, 0.15f, 0.60f, 0.15f, 0.0f, 0.95f);
    }
    return false;
}

static Color icon_color(Icon k)
{
    switch (k) {
    case ICON_CROSS:    { Color c = { 120, 170, 255 }; return c; }   // синий
    case ICON_CIRCLE:   { Color c = { 255, 100, 100 }; return c; }   // красный
    case ICON_SQUARE:   { Color c = { 240, 130, 200 }; return c; }   // розовый
    case ICON_TRIANGLE: { Color c = {  90, 210, 170 }; return c; }   // зелёный
    default:            { Color c = { 200, 204, 212 }; return c; }
    }
}

// top - верхняя граница иконки. Края сглаживаются выборкой 4x4 точки на пиксель.
static void draw_icon(Icon k, int x, int top, Color bg)
{
    Color fg = icon_color(k);
    for (int py = 0; py < ICON_S; py++) {
        for (int px = 0; px < ICON_S; px++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float u = ((px + (sx + 0.5f) / 4.0f) / ICON_S) * 2.0f - 1.0f;
                    float v = ((py + (sy + 0.5f) / 4.0f) / ICON_S) * 2.0f - 1.0f;
                    if (icon_hit(k, u, v)) hit++;
                }
            if (hit == 0) continue;
            int xx = x + px, yy = top + py;
            if (xx < 0 || yy < 0 || xx >= FRAME_WIDTH || yy >= FRAME_HEIGHT) continue;
            Color c;
            c.r = (uint8_t)((bg.r * (16 - hit) + fg.r * hit) / 16);
            c.g = (uint8_t)((bg.g * (16 - hit) + fg.g * hit) / 16);
            c.b = (uint8_t)((bg.b * (16 - hit) + fg.b * hit) / 16);
            g_scene->PutPixel(xx, yy, c);
        }
    }
}

struct RichTok { bool isIcon; Icon icon; std::string text; };

static bool icon_by_name(const std::string& n, Icon& out)
{
    if (n == "cross")    { out = ICON_CROSS;    return true; }
    if (n == "circle")   { out = ICON_CIRCLE;   return true; }
    if (n == "square")   { out = ICON_SQUARE;   return true; }
    if (n == "triangle") { out = ICON_TRIANGLE; return true; }
    if (n == "options")  { out = ICON_OPTIONS;  return true; }
    if (n == "updown")   { out = ICON_UPDOWN;   return true; }
    return false;
}

static std::vector<RichTok> rich_parse(const std::string& s)
{
    std::vector<RichTok> out;
    std::string cur;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '{') {
            size_t e = s.find('}', i);
            Icon k;
            if (e != std::string::npos && icon_by_name(s.substr(i + 1, e - i - 1), k)) {
                if (!cur.empty()) { RichTok t; t.isIcon = false; t.icon = ICON_CROSS; t.text = cur; out.push_back(t); cur.clear(); }
                RichTok t; t.isIcon = true; t.icon = k; out.push_back(t);
                i = e + 1;
                continue;
            }
        }
        cur += s[i++];
    }
    if (!cur.empty()) { RichTok t; t.isIcon = false; t.icon = ICON_CROSS; t.text = cur; out.push_back(t); }
    return out;
}

static int rich_width(const std::vector<RichTok>& toks)
{
    int w = 0;
    for (size_t i = 0; i < toks.size(); i++) w += toks[i].isIcon ? ICON_S : count_cp(toks[i].text) * g_cellW;
    return w;
}

static void put_center_rich(int cx, const std::string& s, int baseline, Color fg, Color bg)
{
    std::vector<RichTok> toks = rich_parse(s);
    int x = cx - rich_width(toks) / 2;
    for (size_t i = 0; i < toks.size(); i++) {
        if (toks[i].isIcon) { draw_icon(toks[i].icon, x, baseline - 22, bg); x += ICON_S; }
        else { draw_text(toks[i].text, x, baseline, fg, bg); x += count_cp(toks[i].text) * g_cellW; }
    }
}

static int col_x(int col) { return MARGIN_X + col * g_cellW; }

static void put_left(int col, const std::string& s, int baseline, Color fg, Color bg)
{
    draw_text(s, col_x(col), baseline, fg, bg);
}

static void put_right(int col, int width, const std::string& s, int baseline, Color fg, Color bg)
{
    std::string t = fit(s, width);
    int n = count_cp(t);
    draw_text(t, col_x(col + width - n), baseline, fg, bg);
}

static void put_center(int cx, const std::string& s, int baseline, Color fg, Color bg)
{
    int w = count_cp(s) * g_cellW;
    draw_text(s, cx - w / 2, baseline, fg, bg);
}

// Рисует картинку (ближайший сосед, с масштабированием) с альфа-смешением по фону bg.
// Фон передаём явно (как и в draw_text), потому что читать обратно из видеопамяти накладно.
static void draw_image(const Image& img, int dstX, int dstY, int dstW, int dstH, Color bg)
{
    if (!img.pixels || img.width <= 0 || img.height <= 0 || dstW <= 0 || dstH <= 0) return;

    for (int y = 0; y < dstH; y++) {
        int sy = y * img.height / dstH;
        int py = dstY + y;
        if (py < 0 || py >= FRAME_HEIGHT) continue;

        for (int x = 0; x < dstW; x++) {
            int sx = x * img.width / dstW;
            int px = dstX + x;
            if (px < 0 || px >= FRAME_WIDTH) continue;

            const unsigned char* s = img.pixels + ((size_t)sy * img.width + sx) * 4;
            unsigned a = s[3];
            if (a == 0) continue;

            Color c;
            c.r = (uint8_t)((bg.r * (255 - a) + s[0] * a) / 255);
            c.g = (uint8_t)((bg.g * (255 - a) + s[1] * a) / 255);
            c.b = (uint8_t)((bg.b * (255 - a) + s[2] * a) / 255);
            g_scene->PutPixel(px, py, c);
        }
    }
}

// ---------------------------------------------------------------- форматирование

static std::string fmt_size(uint64_t b)
{
    char buf[40];
    if (b >= 1073741824ULL)      snprintf(buf, sizeof(buf), "%.1f", b / 1073741824.0);
    else if (b >= 1048576ULL)    snprintf(buf, sizeof(buf), "%.1f", b / 1048576.0);
    else if (b >= 1024ULL)       snprintf(buf, sizeof(buf), "%.0f", b / 1024.0);
    else                         snprintf(buf, sizeof(buf), "%llu", (unsigned long long)b);
    if (b >= 1073741824ULL)      return std::string(buf) + tr("tc_u_gb", "GB");      // гигабайты вплотную к числу
    if (b >= 1048576ULL)         return std::string(buf) + " " + tr("tc_u_mb", "MB");
    if (b >= 1024ULL)            return std::string(buf) + " " + tr("tc_u_kb", "KB");
    return std::string(buf) + " " + tr("tc_u_b", "B");
}

static std::string fmt_speed(int kbps)
{
    char buf[40];
    if (kbps >= 1024) { snprintf(buf, sizeof(buf), "%.1f", kbps / 1024.0); return std::string(buf) + " " + tr("tc_u_mbs", "MB/s"); }
    snprintf(buf, sizeof(buf), "%d", kbps);
    return std::string(buf) + " " + tr("tc_u_kbs", "KB/s");
}

static std::string fmt_eta(long long s)
{
    if (s <= 0) return "-";   // неизвестно, или уже готово (статус и так это покажет)
    std::string h = tr("tc_u_h", "h"), m = tr("tc_u_m", "m"), d = tr("tc_u_d", "d");
    if (s < 3600)  return std::to_string(s / 60 + (s % 60 ? 1 : 0)) + m;
    if (s < 86400) return std::to_string(s / 3600) + h + std::to_string((s % 3600) / 60) + m;
    return std::to_string(s / 86400) + d;
}

static std::string status_label(const std::string& s)
{
    if (s == "checking")    return tr("tc_st_checking", "checking");
    if (s == "downloading") return tr("tc_st_downloading", "downloading");
    if (s == "paused")      return tr("tc_st_paused", "paused");
    if (s == "complete")    return tr("tc_st_complete", "complete");
    if (s == "offline")     return tr("tc_st_offline", "drive gone");
    if (s == "waiting")     return tr("tc_st_waiting", "waiting");
    if (s == "queued")      return tr("tc_st_queued", "queued");
    return s;
}

static Color status_color(const std::string& s)
{
    if (s == "complete")    return C_GREEN;
    if (s == "downloading") return C_TEXT;
    if (s == "offline")     return C_RED;
    if (s == "waiting")     return C_AMBER;
    if (s == "paused")      return C_AMBER;
    return C_DIM;
}

// ---------------------------------------------------------------- отрисовка

static void draw_row(int vis, int idx)
{
    const Item& it = g_status.items[idx];
    bool selected = (idx == g_sel);
    int top = ROWS_Y + vis * ROW_H;
    Color bg = selected ? C_SEL : ((idx % 2) ? C_ROW2 : C_BG);
    g_scene->FillRect(0, top, FRAME_WIDTH, ROW_H, bg);
    int base = top + 25;

    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%d", idx + 1);
    put_right(COL_NUM, W_NUM, tmp, base, C_DIM, bg);

    put_left(COL_NAME, fit(it.title, W_NAME), base, C_TEXT, bg);
    put_right(COL_SIZE, W_SIZE, fmt_size(it.size), base, it.lowSpace ? C_AMBER : C_TEXT, bg);

    char dbuf[64];
    snprintf(dbuf, sizeof(dbuf), "%s (%d%%)", fmt_size(it.done).c_str(), it.pct);
    put_right(COL_DONE, W_DONE, dbuf, base, it.status == "complete" ? C_GREEN : C_TEXT, bg);

    bool showEta = (it.status == "downloading");
    put_right(COL_ETA, W_ETA, showEta ? fmt_eta(it.etaSec) : "-", base, showEta ? C_TEXT : C_DIM, bg);

    std::string stText;
    if (it.status == "downloading") stText = fmt_speed(it.speedKB);
    else                            stText = status_label(it.status);
    put_right(COL_SPEED, W_SPEED, stText, base, status_color(it.status), bg);

    if (it.status == "downloading") snprintf(tmp, sizeof(tmp), "%d", it.peers);
    else                             snprintf(tmp, sizeof(tmp), "-");
    put_right(COL_PEERS, W_PEERS, tmp, base, C_TEXT, bg);

    // Тонкая полоска прогресса под строкой: от начала названия до правого края колонки Status
    // (Peers в неё не входит). Серая "дорожка", голубая заливка по проценту.
    int barX0 = col_x(COL_NAME);
    int barX1 = col_x(COL_SPEED + W_SPEED);
    int barW = barX1 - barX0;
    int pct = it.pct;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int barY = top + ROW_H - BAR_GAP - BAR_H;
    g_scene->FillRect(barX0, barY, barW, BAR_H, C_TRACK);
    int fillW = barW * pct / 100;
    if (fillW > 0) g_scene->FillRect(barX0, barY, fillW, BAR_H, C_FILL);
}

// Делит текст на строки: по \n и по словам, чтобы строка не длиннее maxChars символов.
static std::vector<std::string> wrap_lines(const std::string& text, int maxChars)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string para = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (para.empty()) {
            out.push_back("");
        } else {
            std::string line;
            size_t i = 0;
            while (i < para.size()) {
                size_t sp = para.find(' ', i);
                std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
                i = (sp == std::string::npos) ? para.size() : sp + 1;
                if (word.empty()) continue;
                if (!line.empty() && count_cp(line) + 1 + count_cp(word) > maxChars) { out.push_back(line); line.clear(); }
                line += (line.empty() ? "" : " ") + word;
            }
            if (!line.empty()) out.push_back(line);
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

static void draw_panel(int w, int h)
{
    int x = (FRAME_WIDTH - w) / 2;
    int y = (FRAME_HEIGHT - h) / 2;
    g_scene->FillRect(x - 3, y - 3, w + 6, h + 6, C_EDGE);
    g_scene->FillRect(x, y, w, h, C_PANEL);
}

static void draw_quit_modal()
{
    const int W = 1200;
    int maxChars = (W - 100) / g_cellW;
    std::vector<std::string> lines = wrap_lines(tr("tc_quit_desc",
        "All downloads will stop and progress is saved.\n"
        "The payload will need to be restarted - ps4torrentd.elf"), maxChars);

    const int LINE_H = 34;
    int H = 70 + 20 + (int)lines.size() * LINE_H + 40 + 50;
    draw_panel(W, H);
    int cx = FRAME_WIDTH / 2;
    int y0 = (FRAME_HEIGHT - H) / 2;

    put_center(cx, fit(tr("quit_title", "Shut down ps4torrent"), maxChars), y0 + 50, C_RED, C_PANEL);
    int y = y0 + 70 + 20 + 26;
    for (size_t i = 0; i < lines.size(); i++, y += LINE_H)
        put_center(cx, lines[i], y, C_TEXT, C_PANEL);
    put_center_rich(cx, "{cross} " + tr("btn_quit", "shut down") + "   {circle} " + tr("tc_cancel", "cancel"),
                    y0 + H - 36, C_AMBER, C_PANEL);
}

static void draw_modal()
{
    if (g_modal == MODAL_QUIT) { draw_quit_modal(); return; }
    if (g_modal != MODAL_DELETE) return;

    const int W = 1200, H = 300;
    draw_panel(W, H);
    int cx = FRAME_WIDTH / 2;
    int y0 = (FRAME_HEIGHT - H) / 2;

    put_center(cx, fit(tr("tc_remove_title", "Remove this torrent?"), 64), y0 + 50, C_TEXT, C_PANEL);
    put_center(cx, fit(g_modalTitle, 64), y0 + 100, C_DIM, C_PANEL);
    put_center(cx, fit(tr("tc_remove_note", "Downloaded files stay on the drive unless you choose to delete them too"), 74), y0 + 160, C_AMBER, C_PANEL);
    put_center_rich(cx, "{cross} " + tr("tc_remove", "remove") + "   {square} " + tr("tc_remove_files", "remove + files") + "   {circle} " + tr("tc_cancel", "cancel"),
                    y0 + 230, C_TEXT, C_PANEL);
}

// Окно с состоянием запуска службы (пока она не готова): проверка, отправка, ошибка с подсказкой, что делать.
static void draw_svc_panel()
{
    if (g_svc == SVC_READY || g_modal != MODAL_NONE) return;

    std::string m1, m2;
    Color c1 = C_TEXT;
    if (g_svc == SVC_CHECK)        m1 = tr("tc_svc_checking", "Checking ps4torrentd...");
    else if (g_svc == SVC_WAITING) m1 = tr("tc_svc_starting", "Starting ps4torrentd (sending it to BinLoader)...");
    else {
        m1 = g_svcErr;
        c1 = C_RED;
        m2 = "{cross} " + tr("tc_hint_retry", "try again");
    }

    const int W = 1500, H = 220;
    draw_panel(W, H);
    int cx = FRAME_WIDTH / 2;
    int y0 = (FRAME_HEIGHT - H) / 2;
    std::vector<std::string> lines = wrap_lines(m1, (W - 60) / g_cellW);
    int y = y0 + (m2.empty() ? 120 : 80);
    for (size_t i = 0; i < lines.size() && i < 3; i++, y += 38) put_center(cx, lines[i], y, c1, C_PANEL);
    if (!m2.empty()) put_center_rich(cx, m2, y0 + H - 36, C_TEXT, C_PANEL);
}

static void draw_all()
{
    g_scene->FillRect(0, 0, FRAME_WIDTH, FRAME_HEIGHT, C_BG);

    int statusX;
    // Баннер занимает BANNER_TOP..BANNER_TOP+BANNER_H; строку статуса центрируем в нём по высоте,
    // а не прижимаем к низу, иначе рядом с крупным логотипом она выглядит мелкой и прижатой.
    int statusBaseline = BANNER_TOP + BANNER_H / 2 + 8;
    if (g_bannerOk) {
        draw_image(g_banner, BANNER_LEFT, BANNER_TOP, g_bannerW, BANNER_H, C_BG);
        statusX = BANNER_LEFT + g_bannerW + 28;
    } else {
        statusX = col_x(0);
        statusBaseline = BANNER_TOP + 25;
    }

    {
        // Первый элемент строки показывает состояние связи, три варианта:
        //  - есть и 127.0.0.1, и настоящая сеть  -> IP:port (как раньше)
        //  - 127.0.0.1 отвечает, а сети нет       -> NO INTERNET (жёлтым)
        //  - 127.0.0.1 не отвечает дольше 10 с    -> NO PAYLOAD (красным)
        // Короткие промахи (пока ps4torrentd пересоздаёт веб-сокет после смены сети) НЕ считаются:
        // в это время держим последнее известное состояние, см. connstate.h.
        // Диски/скорость/пиры показываем всегда по последним известным данным.
        ConnState cs = conn_state(g_status.ok, g_lastOkUs, sceKernelGetProcessTime(), g_status.netIp);

        std::string line;
        Color lineColor;
        if (cs == CONN_NO_PAYLOAD) {
            line = tr("tc_no_payload", "NO PAYLOAD");
            lineColor = C_RED;
        } else if (cs == CONN_NO_INTERNET) {
            line = tr("tc_no_internet", "NO INTERNET");
            lineColor = C_AMBER;
        } else {
            line = g_status.netIp + ":" + std::to_string(g_port);
            lineColor = C_TEXT;
            bool anyLow = false;
            for (size_t i = 0; i < g_status.drives.size(); i++) if (g_status.drives[i].low) anyLow = true;
            if (anyLow) lineColor = C_AMBER;
        }

        for (size_t i = 0; i < g_status.drives.size(); i++) {
            const Drive& d = g_status.drives[i];
            if (!line.empty()) line += " | ";
            line += (d.isInternal ? "HDD: " : "USB: ");
            line += fmt_size(d.freeBytes) + " " + tr("tc_free", "free");
        }

        line += " | " + fmt_speed(g_status.totalSpeedKB);

        int maxChars = (FRAME_WIDTH - MARGIN_X - statusX) / g_cellW;
        draw_text(fit(line, maxChars), statusX, statusBaseline, lineColor, C_BG);
    }

    g_scene->FillRect(0, HEADER_Y, FRAME_WIDTH, ROW_H, C_HEAD);
    int hb = HEADER_Y + 25;
    put_right(COL_NUM, W_NUM, "#", hb, C_DIM, C_HEAD);
    put_left(COL_NAME, tr("tc_hdr_title", "Title"), hb, C_DIM, C_HEAD);
    put_right(COL_SIZE, W_SIZE, tr("tc_hdr_size", "Size"), hb, C_DIM, C_HEAD);
    put_right(COL_DONE, W_DONE, tr("tc_hdr_done", "Done"), hb, C_DIM, C_HEAD);
    put_right(COL_ETA, W_ETA, tr("tc_hdr_eta", "ETA"), hb, C_DIM, C_HEAD);
    put_right(COL_SPEED, W_SPEED, tr("tc_hdr_status", "Status"), hb, C_DIM, C_HEAD);
    put_right(COL_PEERS, W_PEERS, tr("tc_hdr_peers", "Peers"), hb, C_DIM, C_HEAD);

    ConnState csNow = conn_state(g_status.ok, g_lastOkUs, sceKernelGetProcessTime(), g_status.netIp);
    bool svcDown = (g_svc == SVC_READY && csNow == CONN_NO_PAYLOAD);   // служба была и пропала (выключена, упала)
    if (g_status.items.empty() && g_svc == SVC_READY) {
        std::string msg = g_status.ok ? tr("tc_empty", "No torrents yet. Drop a .torrent on the USB drive or in /data/pkg/torrents")
                                      : (svcDown ? tr("tc_svc_down", "ps4torrentd is not running")
                                                 : tr("tc_waiting", "Waiting for the first reply from ps4torrentd..."));
        put_center(FRAME_WIDTH / 2, msg, 460, svcDown ? C_AMBER : C_DIM, C_BG);
    }

    for (int v = 0; v < VISIBLE; v++) {
        int idx = g_top + v;
        if (idx >= (int)g_status.items.size()) break;
        draw_row(v, idx);
    }

    static const int HINT_BASELINE   = FOOTER_Y + 25;         // 1025
    static const int CREDIT_BASELINE = HINT_BASELINE + 32;    // 1057: с запасом до низа экрана (1080)

    std::string hint;
    if (g_svc == SVC_FAILED)  hint = "{cross} " + tr("tc_hint_retry", "try again");
    else if (svcDown)         hint = "{cross} " + tr("tc_hint_start", "start the service");
    else if (g_svc == SVC_READY)
        hint = "{updown} " + tr("tc_hint_select", "select") + "   {cross} " + tr("tc_hint_toggle", "pause/resume") + "   {triangle} " + tr("tc_hint_remove", "remove") + "   {options} " + tr("btn_quit", "shut down");
    if (!hint.empty()) put_center_rich(FRAME_WIDTH / 2, hint, HINT_BASELINE, C_DIM, C_BG);

    put_center(FRAME_WIDTH / 2, "ps4torrent client v" APP_VERSION " Created by SergioPoverony and Mr.Claude",
               CREDIT_BASELINE, C_DIMMER, C_BG);

    draw_svc_panel();

    draw_modal();
}

// ---------------------------------------------------------------- геймпад

static int g_pad = -1;

static bool pad_init()
{
    if (scePadInit() != 0) return false;

    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);

    int userID = 0;
    sceUserServiceGetInitialUser(&userID);

    g_pad = scePadOpen(userID, 0, 0, NULL);
    return g_pad >= 0;
}

static unsigned pad_read()
{
    if (g_pad < 0) return 0;
    OrbisPadData data;
    memset(&data, 0, sizeof(data));
    scePadReadState(g_pad, &data);
    return (unsigned)data.buttons;
}

static bool step_repeat(bool down, uint64_t& since, uint64_t& last, uint64_t now)
{
    if (!down) { since = 0; return false; }
    if (since == 0) { since = now; last = now; return true; }
    if (now - since > 400000 && now - last >= 70000) { last = now; return true; }
    return false;
}

// Обновляет данные с сервера и запоминает момент последнего УСПЕШНОГО ответа (нужен для
// "запаса времени" перед надписью NO PAYLOAD, см. connstate.h).
static bool refresh_status()
{
    bool ok = api_refresh_status();
    if (ok) g_lastOkUs = sceKernelGetProcessTime();
    return ok;
}

// ---------------------------------------------------------------- запуск службы

static bool svc_version_ok() { return g_status.ok && g_status.version == APP_VERSION; }

// Ставит службу: копия в автозапуск GoldHEN (payloads.ini) и отправка ELF из PKG в BinLoader (порт 9090).
// Сюда попадаем только когда служба не отвечает или отвечает версией, не равной версии приложения. Если служба уже
// работает нужной версией, ни диск, ни payloads.ini, ни порт 9090 не трогаются.
static void svc_send_now(uint64_t now)
{
    std::string instErr;
    if (!service_install_autorun(instErr)) logf_("svc: autorun install failed: %s", instErr.c_str());

    std::string err;
    bool connectFailed = false;
    if (service_send_payload(err, &connectFailed)) {
        g_svc = SVC_WAITING;
        g_svcDeadline = sceKernelGetProcessTime() + 45ULL * 1000000ULL;
        g_svcErr.clear();
    } else {
        g_svc = SVC_FAILED;
        g_svcErr = connectFailed
            ? tr("err_loader_off", "BinLoader is off. Enable it: GoldHEN - Servers settings - Enable Payloader Server")
            : err;
    }
    (void)now;
    g_redraw = 2;
}

// Раз в секунду, сразу после опроса службы.
static void svc_step(uint64_t now)
{
    if (g_svc == SVC_CHECK) {
        if (g_host != "127.0.0.1") { g_svc = SVC_READY; return; }       // служба на другой консоли: запускать нечего
        if (g_status.ok) {
            if (svc_version_ok()) {
                logf_("svc: service %s already running", g_status.version.c_str());
                g_svc = SVC_READY;
            } else {
                logf_("svc: running service is '%s', app is %s: replacing", g_status.version.c_str(), APP_VERSION);
                svc_send_now(now);
            }
        } else if (++g_svcChecks >= 3) {      // три секунды тишины подряд: службы нет
            logf_("svc: service not running, starting it");
            svc_send_now(now);
        }
        g_redraw = 2;
    } else if (g_svc == SVC_WAITING) {
        if (svc_version_ok()) {
            logf_("svc: service is up (%s)", g_status.version.c_str());
            g_svc = SVC_READY;
            g_redraw = 2;
        } else if (now > g_svcDeadline) {
            logf_("svc: service did not come up in time");
            g_svc = SVC_FAILED;
            g_svcErr = tr("tc_svc_noanswer", "The service did not answer");
            g_redraw = 2;
        }
    } else if (g_svc == SVC_FAILED) {
        if (svc_version_ok()) {               // поднялась с опозданием
            logf_("svc: service came up late (%s)", g_status.version.c_str());
            g_svc = SVC_READY;
            g_redraw = 2;
        }
    }
}

// Cross запускает службу заново: после ошибки или когда служба пропала (выключена, упала).
static bool svc_can_start()
{
    if (g_host != "127.0.0.1") return false;
    if (g_svc == SVC_FAILED) return true;
    return g_svc == SVC_READY &&
           conn_state(g_status.ok, g_lastOkUs, sceKernelGetProcessTime(), g_status.netIp) == CONN_NO_PAYLOAD;
}

// ---------------------------------------------------------------- управление раздачей

static void toggle_selected()
{
    if (g_sel < 0 || g_sel >= (int)g_status.items.size()) return;
    const Item& it = g_status.items[g_sel];
    if (it.status == "complete" || it.status == "offline") return;

    if (it.paused) api_resume(it.hash);
    else           api_pause(it.hash);

    refresh_status();   // сразу подтягиваем новое состояние, не дожидаясь таймера
    g_redraw = 2;
}

// ---------------------------------------------------------------- публичные функции

bool ui_init()
{
    g_scene = new Scene2D(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);
    if (!g_scene->Init(0xC000000, 2)) {
        logf_("ui: scene init FAILED, working without screen");
        return false;
    }

    const char* fontPath = "/app0/assets/fonts/DejaVuSansMono.ttf";
    if (!g_scene->InitFont(&g_font, fontPath, FONT_PX)) {
        logf_("ui: font init FAILED (%s), working without screen", fontPath);
        return false;
    }

    FT_UInt gi = FT_Get_Char_Index(g_font, 'M');
    if (!FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) {
        int w = (int)(g_font->glyph->advance.x >> 6);
        if (w > 0) g_cellW = w;
    }

    g_bannerOk = g_banner.load("/app0/assets/images/title.png");
    if (g_bannerOk) {
        g_bannerW = BANNER_H * g_banner.width / g_banner.height;
        logf_("ui: banner loaded, %dx%d -> %dx%d on screen", g_banner.width, g_banner.height, g_bannerW, BANNER_H);
    } else {
        logf_("ui: no banner image, using text title instead");
    }

    g_padOk = pad_init();
    logf_("ui: ready, cell width %d, visible rows %d, pad %s", g_cellW, VISIBLE, g_padOk ? "ok" : "FAILED");

    g_lastTick = sceKernelGetProcessTime();
    g_lastRefresh = 0;
    g_ready = true;
    return true;
}

void ui_mark_dirty() { g_redraw = 2; }

void ui_pump()
{
    if (!g_ready) return;

    uint64_t now = sceKernelGetProcessTime();

    // Раз в секунду обновляем список с сервера.
    if (now - g_lastRefresh >= 1000000) {
        g_lastRefresh = now;
        refresh_status();
        svc_step(now);
        i18n_tick(g_status.ok, now);
        g_redraw = 2;
    }

    unsigned b = pad_read();
    static unsigned prev = 0;
    unsigned pressed = b & ~prev;
    prev = b;

    bool up       = (b & ORBIS_PAD_BUTTON_UP) != 0;
    bool down     = (b & ORBIS_PAD_BUTTON_DOWN) != 0;
    bool cross    = (pressed & ORBIS_PAD_BUTTON_CROSS) != 0;
    bool circle   = (pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0;
    bool triangle = (pressed & ORBIS_PAD_BUTTON_TRIANGLE) != 0;
    bool square   = (pressed & ORBIS_PAD_BUTTON_SQUARE) != 0;
    bool options  = (pressed & ORBIS_PAD_BUTTON_OPTIONS) != 0;

    static uint64_t upSince = 0, upLast = 0, downSince = 0, downLast = 0;

    int n = (int)g_status.items.size();

    if (g_modal == MODAL_DELETE) {
        upSince = downSince = 0;
        if (cross)       { api_delete(g_modalHash, false); refresh_status(); g_modal = MODAL_NONE; g_redraw = 2; }
        else if (square)  { api_delete(g_modalHash, true);  refresh_status(); g_modal = MODAL_NONE; g_redraw = 2; }
        else if (circle)  { g_modal = MODAL_NONE; g_redraw = 2; }
    } else if (g_modal == MODAL_QUIT) {
        upSince = downSince = 0;
        if (cross) {
            api_quit();            // демон сохраняет прогресс и завершается; дальше шапка покажет NO PAYLOAD
            g_modal = MODAL_NONE;
            g_redraw = 2;
        } else if (circle) { g_modal = MODAL_NONE; g_redraw = 2; }
    } else {
        if (options && g_svc == SVC_READY) {
            g_modal = MODAL_QUIT;
            g_redraw = 2;
        } else if (cross && svc_can_start()) {
            logf_("svc: start requested from the gamepad");
            svc_send_now(now);
        } else if (n > 0 && g_svc == SVC_READY) {
            if (step_repeat(up, upSince, upLast, now) && g_sel > 0)           { g_sel--; g_redraw = 2; }
            if (step_repeat(down, downSince, downLast, now) && g_sel + 1 < n) { g_sel++; g_redraw = 2; }

            if (cross) toggle_selected();
            if (triangle) {
                g_modalHash = g_status.items[g_sel].hash;
                g_modalTitle = g_status.items[g_sel].title;
                g_modal = MODAL_DELETE;
                g_redraw = 2;
            }
        }
    }

    if (g_sel >= n) g_sel = (n > 0) ? n - 1 : 0;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + VISIBLE) g_top = g_sel - VISIBLE + 1;
    if (g_top < 0) g_top = 0;

    if (now - g_lastTick >= 1000000) { g_lastTick = now; g_redraw = 2; }

    if (g_redraw > 0) {
        draw_all();
        g_scene->SubmitFlip(g_frameID);
        g_scene->FrameWait(g_frameID);
        g_scene->FrameBufferSwap();
        g_frameID++;
        g_redraw--;
    }
}
