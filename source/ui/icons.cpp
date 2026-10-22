// Vector icons drawn with the primitives in gfx.cpp. Each is designed on a
// 24x24 grid centred on (cx, cy).
#include "app.h"
#include <cmath>

void drawIcon(int icon, float cx, float cy, float size, SDL_Color c) {
    const float u = size / 24.0f;
    auto X = [&](float x) { return cx + (x - 12) * u; };
    auto Y = [&](float y) { return cy + (y - 12) * u; };
    auto line = [&](float x1, float y1, float x2, float y2, float t = 2) { drawLine(X(x1), Y(y1), X(x2), Y(y2), t * u, c); };
    auto tri = [&](float x1, float y1, float x2, float y2, float x3, float y3) { fillTri(X(x1), Y(y1), X(x2), Y(y2), X(x3), Y(y3), c); };
    auto rect = [&](float x, float y, float w, float h, float r = 0) {
        fillRound(SDL_Rect{(int)X(x), (int)Y(y), (int)(w * u + 0.5f), (int)(h * u + 0.5f)}, (int)(r * u), c);
    };
    auto frame = [&](float x, float y, float w, float h, float r, float t = 2) {
        strokeRound(SDL_Rect{(int)X(x), (int)Y(y), (int)(w * u + 0.5f), (int)(h * u + 0.5f)}, (int)(r * u), (int)std::max(1.0f, t * u), c);
    };
    auto arc = [&](float x, float y, float r, float a0deg, float a1deg, float t = 2) {
        drawArc(X(x), Y(y), r * u, a0deg * (float)M_PI / 180, a1deg * (float)M_PI / 180, t * u, c);
    };
    auto disc = [&](float x, float y, float r) { fillCircle(X(x), Y(y), r * u, c); };
    auto ring = [&](float x, float y, float r, float t = 2) { strokeCircle(X(x), Y(y), r * u, t * u, c); };
    auto arrowHead = [&](float x, float y, float angDeg, float len) {
        float a = angDeg * (float)M_PI / 180;
        float bx = x - cosf(a) * len, by = y - sinf(a) * len;
        float px = -sinf(a) * len * 0.75f, py = cosf(a) * len * 0.75f;
        tri(x, y, bx + px, by + py, bx - px, by - py);
    };
    (void)tri;

    switch (icon) {
        case IC_SYNC:
        case IC_REFRESH:
            arc(12, 12, 7.5f, 200, 340);
            arrowHead(19.2f, 9.6f, 70, 4);
            arc(12, 12, 7.5f, 20, 160);
            arrowHead(4.8f, 14.4f, 250, 4);
            break;
        case IC_MEDIA:
            frame(3, 5, 18, 14, 2.5f);
            disc(9, 10, 1.8f);
            tri(6, 17, 11, 11.5f, 15, 17);
            tri(11, 17, 15, 12.5f, 19, 17);
            break;
        case IC_IMAGE:
            frame(3, 4, 18, 16, 2.5f);
            disc(8.5f, 9.5f, 2);
            tri(5.5f, 17.5f, 11, 11, 15, 17.5f);
            tri(11.5f, 17.5f, 15.5f, 13, 19, 17.5f);
            break;
        case IC_VIDEO:
            frame(2.5f, 5, 19, 14, 2.5f);
            tri(10, 8.5f, 10, 15.5f, 16, 12);
            break;
        case IC_MUSIC:
            disc(7.5f, 17, 3);
            disc(17, 15, 3);
            line(10.2f, 17, 10.2f, 5.5f);
            line(19.7f, 15, 19.7f, 3.5f);
            line(10.2f, 5.5f, 19.7f, 3.5f, 2.6f);
            break;
        case IC_BOOK:
            frame(3, 4.5f, 8.5f, 15, 1.5f);
            frame(12.5f, 4.5f, 8.5f, 15, 1.5f);
            line(5.5f, 9, 9, 9, 1.4f);
            line(5.5f, 12, 9, 12, 1.4f);
            line(15, 9, 18.5f, 9, 1.4f);
            line(15, 12, 18.5f, 12, 1.4f);
            break;
        case IC_COMIC:
            frame(3, 3, 18, 18, 2);
            line(12, 3.5f, 12, 11.5f);
            line(3.5f, 11.5f, 20.5f, 11.5f);
            line(9, 11.5f, 9, 20.5f);
            break;
        case IC_FOLDER:
            rect(2.5f, 5, 8, 4, 1.5f);
            rect(2.5f, 7.5f, 19, 12, 2);
            break;
        case IC_FILE:
            frame(5, 2.5f, 14, 19, 2);
            line(8, 9, 16, 9, 1.5f);
            line(8, 12.5f, 16, 12.5f, 1.5f);
            line(8, 16, 13, 16, 1.5f);
            break;
        case IC_PLAY: tri(7, 4, 7, 20, 20, 12); break;
        case IC_PAUSE:
            rect(6, 4.5f, 4, 15, 1);
            rect(14, 4.5f, 4, 15, 1);
            break;
        case IC_PAUSECIRCLE:
            ring(12, 12, 10);
            rect(8.5f, 7.5f, 2.5f, 9, 0.8f);
            rect(13, 7.5f, 2.5f, 9, 0.8f);
            break;
        case IC_NEXT:
            tri(5, 5, 5, 19, 16, 12);
            rect(16.5f, 5, 2.8f, 14, 1);
            break;
        case IC_PREV:
            tri(19, 5, 19, 19, 8, 12);
            rect(4.7f, 5, 2.8f, 14, 1);
            break;
        case IC_FWD:
        case IC_RWD: {
            bool fwd = icon == IC_FWD;
            if (fwd) { arc(12, 13, 7.5f, 160, 380); arrowHead(19.4f, 10.5f, 80, 4.2f); }
            else { arc(12, 13, 7.5f, -200, 20); arrowHead(4.6f, 10.5f, 100, 4.2f); }
            drawText("10", (int)X(12), (int)Y(9.2f), std::max(8, (int)(7.5f * u)), c, 1, 0, FS_BOLD);
            break;
        }
        case IC_BACK:
        case IC_LEFT:
            line(15, 4.5f, 7.5f, 12, 2.4f);
            line(7.5f, 12, 15, 19.5f, 2.4f);
            break;
        case IC_RIGHT:
            line(9, 4.5f, 16.5f, 12, 2.4f);
            line(16.5f, 12, 9, 19.5f, 2.4f);
            break;
        case IC_UP:
            line(4.5f, 15, 12, 7.5f, 2.4f);
            line(12, 7.5f, 19.5f, 15, 2.4f);
            break;
        case IC_DOWN:
            line(4.5f, 9, 12, 16.5f, 2.4f);
            line(12, 16.5f, 19.5f, 9, 2.4f);
            break;
        case IC_CLOSE:
            line(6, 6, 18, 18, 2.4f);
            line(18, 6, 6, 18, 2.4f);
            break;
        case IC_PLUS:
            line(12, 5, 12, 19, 2.4f);
            line(5, 12, 19, 12, 2.4f);
            break;
        case IC_MINUS: line(5, 12, 19, 12, 2.4f); break;
        case IC_CHECK:
            line(5, 12.5f, 10, 17.5f, 2.6f);
            line(10, 17.5f, 19.5f, 7, 2.6f);
            break;
        case IC_GEAR: {
            for (int i = 0; i < 8; i++) {
                float a = i * (float)M_PI / 4;
                line(12 + cosf(a) * 6.5f, 12 + sinf(a) * 6.5f, 12 + cosf(a) * 9.6f, 12 + sinf(a) * 9.6f, 3.2f);
            }
            ring(12, 12, 6.2f, 3);
            ring(12, 12, 2.2f, 1.8f);
            break;
        }
        case IC_ROTL:
            arc(12, 13, 7, 200, 450);
            arrowHead(4.7f, 9.8f, 285, 4.2f);
            break;
        case IC_ROTR:
            arc(12, 13, 7, 90, 340);
            arrowHead(19.3f, 9.8f, 255, 4.2f);
            break;
        case IC_LIST:
            for (int i = 0; i < 3; i++) {
                disc(5, 6.5f + i * 5.5f, 1.4f);
                line(9, 6.5f + i * 5.5f, 20, 6.5f + i * 5.5f);
            }
            break;
        case IC_SUBS:
            frame(2.5f, 5, 19, 14, 2.5f);
            line(6, 12, 9.5f, 12, 1.8f);
            line(11.5f, 12, 18, 12, 1.8f);
            line(6, 15.5f, 13, 15.5f, 1.8f);
            line(15, 15.5f, 18, 15.5f, 1.8f);
            break;
        case IC_AUDIO:
            rect(3.5f, 9, 4.5f, 6, 1);
            tri(7, 9, 13, 4, 13, 20);
            tri(7, 9, 13, 20, 7, 15);
            arc(13, 12, 4, -50, 50);
            arc(13, 12, 7.5f, -50, 50);
            break;
        case IC_SLIDES:
            frame(2.5f, 4, 19, 13, 2);
            tri(10, 7.5f, 10, 13.5f, 15, 10.5f);
            line(7, 20, 17, 20, 2);
            break;
        case IC_INFO:
            ring(12, 12, 9.5f);
            disc(12, 7.6f, 1.4f);
            line(12, 11, 12, 16.5f, 2.2f);
            break;
        case IC_WARN:
            tri(12, 3, 22, 20.5f, 2, 20.5f);
            fillRect((int)X(11), (int)Y(9), (int)(2 * u), (int)(6 * u), g_theme.bg);
            fillCircle(X(12), Y(17.5f), 1.2f * u, g_theme.bg);
            break;
        case IC_DEVICE:
            frame(4, 4, 16, 11, 1.5f);
            line(2, 18.5f, 22, 18.5f, 2.4f);
            break;
        case IC_QR:
            frame(3, 3, 7.5f, 7.5f, 1);
            frame(13.5f, 3, 7.5f, 7.5f, 1);
            frame(3, 13.5f, 7.5f, 7.5f, 1);
            rect(5.5f, 5.5f, 2.5f, 2.5f);
            rect(16, 5.5f, 2.5f, 2.5f);
            rect(5.5f, 16, 2.5f, 2.5f);
            rect(13.5f, 13.5f, 3, 3);
            rect(18, 18, 3, 3);
            rect(18, 13.5f, 3, 3);
            rect(13.5f, 18, 3, 3);
            break;
        case IC_ZOOM:
            ring(10.5f, 10.5f, 6.5f);
            line(15.5f, 15.5f, 20.5f, 20.5f, 2.6f);
            break;
        case IC_FIT:
            line(4, 9, 4, 4); line(4, 4, 9, 4);
            line(15, 4, 20, 4); line(20, 4, 20, 9);
            line(4, 15, 4, 20); line(4, 20, 9, 20);
            line(20, 15, 20, 20); line(20, 20, 15, 20);
            break;
        case IC_POWER:
            arc(12, 13, 8, -60, 240);
            line(12, 3, 12, 11, 2.4f);
            break;
        case IC_TEXT:
            drawText("A", (int)X(8.5f), (int)Y(3.5f), std::max(8, (int)(17 * u)), c, 1, 0, FS_BOLD);
            drawText("a", (int)X(17), (int)Y(8.5f), std::max(8, (int)(12 * u)), c, 1, 0, FS_BOLD);
            break;
        case IC_LINK:
            frame(2.5f, 8.5f, 11, 7, 3.5f);
            frame(10.5f, 8.5f, 11, 7, 3.5f);
            break;
        case IC_SHUFFLE:
            line(3, 7, 8, 7); line(8, 7, 16, 17); line(16, 17, 19, 17);
            line(3, 17, 8, 17); line(8, 17, 16, 7); line(16, 7, 19, 7);
            arrowHead(21.5f, 7, 0, 4); arrowHead(21.5f, 17, 0, 4);
            break;
        case IC_MOON:
            disc(12, 12, 8.5f);
            fillCircle(X(16), Y(8.5f), 7.5f * u, g_theme.bg);
            break;
        case IC_HOME:
            tri(12, 3.5f, 21, 11.5f, 3, 11.5f);
            rect(5.5f, 11, 13, 9, 1);
            break;
        case IC_LOGOUT:
            frame(3, 4, 10, 16, 2);
            line(10, 12, 20, 12);
            arrowHead(21, 12, 0, 4);
            break;
        case IC_CLOUD:
            disc(8, 14, 4.5f);
            disc(13, 11, 6);
            disc(17.5f, 14.5f, 4);
            rect(8, 14.5f, 9.5f, 4);
            break;
        case IC_TRASH:
            line(4, 6, 20, 6);
            line(9.5f, 6, 10, 3.5f); line(10, 3.5f, 14, 3.5f); line(14, 3.5f, 14.5f, 6);
            frame(6, 7, 12, 14, 2);
            break;
        case IC_EDIT:
            line(5, 19, 17, 7, 3);
            tri(3.5f, 20.5f, 4, 16.5f, 7.5f, 20);
            break;
        case IC_BOOKMARK:
            rect(6, 3, 12, 12, 1);
            tri(6, 14, 18, 14, 6, 21);
            tri(18, 14, 18, 21, 6, 14);
            fillTri(X(6), Y(21.1f), X(18), Y(21.1f), X(12), Y(16), g_theme.bg);
            break;
        case IC_GRID:
            rect(3.5f, 3.5f, 7.5f, 7.5f, 1.5f);
            rect(13, 3.5f, 7.5f, 7.5f, 1.5f);
            rect(3.5f, 13, 7.5f, 7.5f, 1.5f);
            rect(13, 13, 7.5f, 7.5f, 1.5f);
            break;
        case IC_EQ:  // three faders
            line(6, 4, 6, 20); line(12, 4, 12, 20); line(18, 4, 18, 20);
            disc(6, 14.5f, 2.8f); disc(12, 8, 2.8f); disc(18, 16, 2.8f);
            break;
        case IC_LOCK:  // a padlock
            arc(12, 10.5f, 4.5f, 180, 360, 2.2f);
            line(7.5f, 10.5f, 7.5f, 12, 2.2f);
            line(16.5f, 10.5f, 16.5f, 12, 2.2f);
            rect(4.5f, 11.5f, 15, 10, 2);
            break;
        case IC_WAVE:  // spectrum bars
            rect(3, 10, 3, 10, 1.5f);
            rect(8, 4, 3, 16, 1.5f);
            rect(13, 8, 3, 12, 1.5f);
            rect(18, 13, 3, 7, 1.5f);
            break;
        default: break;
    }
}
