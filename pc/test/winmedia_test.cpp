// What Windows does for us: Shell thumbnails, Media Foundation probing and
// waveforms, PDF covers. Shell thumbnails depend on the handlers installed
// (Icaros here); a type with none is skipped with a note rather than failed.
#include <windows.h>

#include "images.h"
#include "library.h"
#include "test.h"
#include "util.h"
#include "winmedia.h"

static std::string fx(const char* name) {
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    return utf8(cwd) + "/test/fixtures/" + name;  // the Shell wants full paths
}

TEST(mf_probe_video_and_audio) {
    MediaInfo v;
    CHECK(mfProbe(fx("black_start.mp4"), v));
    CHECK(v.video);
    CHECK_EQ(v.width, 320);
    CHECK_EQ(v.height, 240);
    CHECK(v.durationMs > 7500 && v.durationMs < 8500);
    MediaInfo r;
    CHECK(mfProbe(fx("rotated.mp4"), r));
    CHECK_EQ(r.width, 240);  // portrait once turned
    CHECK_EQ(r.height, 320);
    MediaInfo a;
    CHECK(mfProbe(fx("tone.wav"), a));
    CHECK(a.audio && !a.video);
    CHECK(a.durationMs > 3900 && a.durationMs < 4100);
}

TEST(mf_waveform) {
    std::vector<uint8_t> peaks;
    int64_t dur = 0;
    CHECK(mfAudioPeaks(fx("tone.wav"), lib::WAVE_PEAKS, peaks, &dur));
    CHECK_EQ((int)peaks.size(), lib::WAVE_PEAKS);
    if (peaks.size() == (size_t)lib::WAVE_PEAKS) {
        CHECK(peaks[10] < 40);    // quiet half
        CHECK(peaks[100] > 200);  // loud half
    }
    CHECK(dur > 3900 && dur < 4100);
    CHECK(mfAudioPeaks(fx("short.wav"), lib::WAVE_PEAKS, peaks, &dur));
    CHECK(dur > 250 && dur < 350);
}

TEST(pdf_cover) {
    Image img;
    CHECK(pdfFirstPage(fx("page.pdf"), 480, img));
    CHECK_EQ(img.h, 480);
    CHECK_EQ(img.w, 320);  // 200 x 300 pt page
    if (img.w == 320) {
        const uint8_t* mid = &img.bgra[((size_t)240 * img.w + 160) * 4];
        CHECK(mid[2] > 200 && mid[1] > 90 && mid[1] < 170 && mid[0] < 60);  // the orange box (BGRA)
        const uint8_t* corner = &img.bgra[((size_t)4 * img.w + 4) * 4];
        CHECK(corner[0] > 240 && corner[1] > 240 && corner[2] > 240);       // white paper
    }
}

// The Shell's thumbnail for a file; flat = it came out one colour (a black frame).
static bool shellCase(const char* file, bool& flat) {
    Image img;
    flat = false;
    if (!shellThumbnail(fx(file), lib::THUMB, img)) {
        printf("  note: no Shell thumbnail for %s (no handler installed?)\n", file);
        return false;
    }
    CHECK(img.w > 0 && img.h > 0);
    CHECK(img.w <= lib::THUMB && img.h <= lib::THUMB);
    double mean, sd;
    imageStats(img, mean, sd);
    flat = sd < 2;
    printf("  %s: %dx%d, brightness %.0f, spread %.1f\n", file, img.w, img.h, mean, sd);
    return true;
}

TEST(shell_thumbnails) {
    bool flat;
    // a clip that opens on black: whatever frame the handler picks, the builder checks it
    shellCase("black_start.mp4", flat);
    if (shellCase("comic.cbz", flat)) CHECK(!flat);
    if (shellCase("book.epub", flat)) CHECK(!flat);
    Image none;
    CHECK(!shellThumbnail(fx("not there.mp4"), 300, none));
}

int main() {
    imagesThreadInit();
    winMediaThreadInit();
    int rc = runTests();
    winMediaThreadExit();
    imagesThreadExit();
    return rc;
}
