// Pictures, ffmpeg analysis and books, on the files in test/fixtures.
#include "books.h"
#include "fsx.h"
#include "images.h"
#include "library.h"
#include "media.h"
#include "test.h"
#include "util.h"

static std::string fx(const char* name) { return std::string("test/fixtures/") + name; }
static FfTools g_ff;

TEST(wic_png_scaled) {
    Image img;
    ImageInfo info;
    CHECK(loadImageFile(fx("big.png"), 360, img, &info));
    CHECK_EQ(info.width, 1200);
    CHECK_EQ(info.height, 800);
    CHECK_EQ(img.w, 360);
    CHECK_EQ(img.h, 240);
    std::vector<uint8_t> jpg;
    CHECK(encodeJpeg(img, 0.85f, jpg));
    CHECK(jpg.size() > 1000 && jpg[0] == 0xFF && jpg[1] == 0xD8);
    Image back;
    CHECK(loadImageMemory(jpg.data(), jpg.size(), 0, back));
    CHECK_EQ(back.w, 360);
}

TEST(probe_and_skip_black) {
    CHECK(g_ff.ok());
    MediaInfo mi;
    CHECK(probeMedia(g_ff, fx("black_start.mp4"), mi));
    CHECK(mi.video);
    CHECK_EQ(mi.width, 320);
    CHECK(mi.durationMs > 7500 && mi.durationMs < 8500);
    Image frame;
    CHECK(videoFrame(g_ff, fx("black_start.mp4"), mi, 200, frame));
    CHECK_EQ(frame.w, 200);
    double mean, sd;
    imageStats(frame, mean, sd);
    CHECK(mean > 24);  // not the black opening
}

TEST(rotated_video_is_portrait) {
    MediaInfo mi;
    CHECK(probeMedia(g_ff, fx("rotated.mp4"), mi));
    CHECK_EQ(mi.width, 240);
    CHECK_EQ(mi.height, 320);
    Image frame;
    CHECK(videoFrame(g_ff, fx("rotated.mp4"), mi, 160, frame));
    CHECK(frame.h > frame.w);  // ffmpeg turned it upright
}

TEST(waveform) {
    MediaInfo mi;
    CHECK(probeMedia(g_ff, fx("tone.wav"), mi));
    CHECK(mi.audio && !mi.video);
    CHECK(mi.durationMs > 3900 && mi.durationMs < 4100);
    std::vector<uint8_t> peaks;
    CHECK(audioPeaks(g_ff, fx("tone.wav"), lib::WAVE_PEAKS, peaks));
    CHECK_EQ((int)peaks.size(), lib::WAVE_PEAKS);
    CHECK(peaks[10] < 40);   // quiet half
    CHECK(peaks[100] > 200); // loud half
    MediaInfo s;
    CHECK(probeMedia(g_ff, fx("short.wav"), s));
    CHECK(s.durationMs > 250 && s.durationMs < 350);
    CHECK(audioPeaks(g_ff, fx("short.wav"), lib::WAVE_PEAKS, peaks));
}

TEST(ffmpeg_decodes_pictures) {
    Image img;
    CHECK(ffmpegImage(g_ff, fx("big.png"), 300, img));
    CHECK_EQ(img.w, 300);
    CHECK_EQ(img.h, 200);
}

TEST(epub_metadata_and_cover) {
    BookInfo b;
    CHECK(readBook(fx("book.epub"), lib::B_EPUB, 300, b));
    CHECK_EQ(b.title, std::string("The Cat & the Hat \xE2\x80\x93 Again"));
    CHECK_EQ(b.author, std::string("Dr. Seuss"));  // not the illustrator
    CHECK_EQ(b.cover.h, 300);
    CHECK_EQ(b.cover.w, 200);
}

TEST(cbz_first_page_naturally) {
    BookInfo b;
    CHECK(readBook(fx("comic.cbz"), lib::B_COMIC, 240, b));
    CHECK_EQ(b.pages, 2);  // not the __MACOSX junk or the text file
    CHECK_EQ(b.cover.h, 240);
    CHECK_EQ(b.title, std::string("comic"));
}

TEST(sort_keys) {
    CHECK(lib::sortKey("Episode 2") < lib::sortKey("Episode 10"));
    CHECK(lib::sortKey("a") < lib::sortKey("B"));
    CHECK(lib::sortKey("007") == lib::sortKey("7"));
}

TEST(xml_and_names) {
    CHECK_EQ(xmlUnescape("a &lt;b&gt; &#65;&#x42; &bogus; &"), std::string("a <b> AB &bogus; &"));
    CHECK_EQ(titleFromFileName("My_Book_Title"), std::string("My Book Title"));
}

// Card names: ASCII only (the Switch can't open other names on its card), stable,
// distinct, and a subtitle still starts with its video's name.
TEST(card_safe_names) {
    CHECK_EQ(cardSafe(".media/files/Pictures/Rain Song.opus"), std::string(".media/files/Pictures/Rain Song.opus"));
    std::string star = cardSafe("Music/Star\xE2\x98\x86Song.opus");  // a white star
    CHECK(star.rfind("Music/Star_Song~", 0) == 0);
    CHECK(star.size() > 4 && star.compare(star.size() - 5, 5, ".opus") == 0);
    for (unsigned char c : star) CHECK(c < 0x80);
    CHECK_EQ(star, cardSafe("Music/Star\xE2\x98\x86Song.opus"));
    CHECK(star != cardSafe("Music/Star\xE2\x98\x85Song.opus"));  // a black star: another name
    // a video and its subtitles, named in Japanese ("name")
    std::string video = cardSafe("\xE5\x90\x8D\xE5\x89\x8D.mkv"), sub = cardSafe("\xE5\x90\x8D\xE5\x89\x8D.ja.srt");
    std::string stem = video.substr(0, video.size() - 4);
    CHECK(sub.rfind(stem + ".", 0) == 0);
    // a long name stays well inside FAT's 255
    std::string longName;
    for (int i = 0; i < 120; i++) longName += "\xE3\x81\x82";
    CHECK(cardSafe(longName + ".mp4").size() < 120);
}

int main() {
    imagesThreadInit();
    g_ff = findFfmpeg("");
    int rc = runTests();
    imagesThreadExit();
    return rc;
}
