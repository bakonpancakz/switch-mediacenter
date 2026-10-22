// Host test for the video file name parser (source/media/vname.cpp).
#include <cstdio>
#include <string>

#include "../../shared/vname.h"

static int fails = 0;

static void ep(const char* rel, const char* show, int season, int episode, int year = 0) {
    VideoName v = parseVideoName(rel);
    bool ok = v.type == VideoName::EPISODE && v.show == show && v.season == season && v.episode == episode && v.year == year;
    printf("%s %s -> episode [%s] s%d e%d y%d\n", ok ? "ok" : "FAIL", rel, v.type == VideoName::EPISODE ? v.show.c_str() : "(movie)",
           v.season, v.episode, v.year);
    if (!ok) { printf("     want [%s] s%d e%d y%d\n", show, season, episode, year); fails++; }
}

static void movie(const char* rel, const char* title, int year) {
    VideoName v = parseVideoName(rel);
    bool ok = v.type == VideoName::MOVIE && v.title == title && v.year == year;
    printf("%s %s -> movie [%s] %d\n", ok ? "ok" : "FAIL", rel, v.type == VideoName::MOVIE ? v.title.c_str() : v.show.c_str(), v.year);
    if (!ok) { printf("     want [%s] %d\n", title, year); fails++; }
}

int main() {
    // scene style
    ep("Breaking.Bad.S01E02.720p.BluRay.x264.mkv", "Breaking Bad", 1, 2);
    ep("The.Office.US.S03E10.WEB-DL.1080p.mkv", "The Office US", 3, 10);
    ep("Show Name - S02E05 - The Title.mkv", "Show Name", 2, 5);
    ep("Severance S02 E03 1080p.mkv", "Severance", 2, 3);
    ep("Doctor.Who.2005.S01E01.Rose.mkv", "Doctor Who", 1, 1, 2005);
    ep("Show (2019) - S01E01.mkv", "Show", 1, 1, 2019);
    ep("show_name_s01e01_multi.mp4", "show name", 1, 1);
    ep("Friends 1x05.avi", "Friends", 1, 5);
    // anime
    ep("[SubsPlease] Bocchi the Rock! - 05 (1080p) [ABCD1234].mkv", "Bocchi the Rock!", -1, 5);
    ep("[Erai-raws] Frieren - 28 [1080p][Multiple Subtitle].mkv", "Frieren", -1, 28);
    ep("Anime/[Judas] Spy x Family - 12v2 [1080p][HEVC x265 10bit].mkv", "Spy x Family", -1, 12);
    ep("One Piece - 1071 [1080p].mkv", "One Piece", -1, 1071);
    ep("[SubsPlease] Oshi no Ko - 03 (1080p).mkv", "Oshi no Ko", -1, 3);
    // folders
    ep("Bocchi the Rock!/Season 1/03.mkv", "Bocchi the Rock!", 1, 3);
    ep("Severance/Season 2/Severance - 04.mkv", "Severance", 2, 4);
    ep("Mushishi/Episode 07.mkv", "Mushishi", -1, 7);
    ep("Cowboy Bebop/Session 05 - Ballad of Fallen Angels [E05].mkv", "Cowboy Bebop", -1, 5);
    ep("Steins Gate/Specials/Steins Gate - 01.mkv", "Steins Gate", 0, 1);
    ep("Show/S2/Show - S02E01.mkv", "Show", 2, 1);
    ep("Planet Earth II/E01 - Islands.mkv", "Planet Earth II", -1, 1);
    // movies
    movie("Spirited Away (2001).mkv", "Spirited Away", 2001);
    movie("Movies/Your.Name.2016.1080p.BluRay.x264.mkv", "Your Name", 2016);
    movie("Blade Runner 2049 (2017).mkv", "Blade Runner 2049", 2017);
    movie("Perfect Blue (1997)/movie.mkv", "Perfect Blue", 1997);
    movie("The Matrix.mp4", "The Matrix", 0);
    movie("Inception.2010.2160p.UHD.BluRay.REMUX.HDR.HEVC.mkv", "Inception", 2010);
    movie("[Group] Akira (1988) [BD 1080p].mkv", "Akira", 1988);

    std::string k = nameKey("Bocchi the Rock!");
    printf("%s nameKey %s\n", k == "bocchitherock" ? "ok" : "FAIL", k.c_str());
    if (k != "bocchitherock") fails++;
    printf("%s\n", fails ? "VNAME FAILED" : "VNAME OK");
    return fails ? 1 : 0;
}
