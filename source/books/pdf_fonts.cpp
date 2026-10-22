// Replaces MuPDF's font table (noto.o in libmupdf.a). Because these functions
// are defined here, the linker never pulls noto.o out of the archive, and the
// ~35 MB of fallback fonts it references (a CJK font alone is 24 MB) stay out
// of the NRO, which is loaded into RAM whole.
//
// Kept: the 14 standard PDF fonts (URW Nimbus clones of Times, Helvetica,
// Courier, Symbol and Dingbats), which PDFs may use without embedding them.
// Dropped: fallbacks for CJK, emoji, maths, music and rare scripts. A PDF that
// uses those without embedding a font shows boxes for those characters; MuPDF
// still renders the rest of the page.
#include <mupdf/fitz.h>

#include <cstring>

#define URW_FONT(NAME)                                \
    extern "C" const unsigned char _binary_##NAME[];  \
    extern "C" const unsigned int _binary_##NAME##_size;
URW_FONT(NimbusMonoPS_Regular_cff)
URW_FONT(NimbusMonoPS_Italic_cff)
URW_FONT(NimbusMonoPS_Bold_cff)
URW_FONT(NimbusMonoPS_BoldItalic_cff)
URW_FONT(NimbusSans_Regular_cff)
URW_FONT(NimbusSans_Italic_cff)
URW_FONT(NimbusSans_Bold_cff)
URW_FONT(NimbusSans_BoldItalic_cff)
URW_FONT(NimbusRoman_Regular_cff)
URW_FONT(NimbusRoman_Italic_cff)
URW_FONT(NimbusRoman_Bold_cff)
URW_FONT(NimbusRoman_BoldItalic_cff)
URW_FONT(StandardSymbolsPS_cff)
URW_FONT(Dingbats_cff)

#define RETURN(NAME) return (*len = (int)_binary_##NAME##_size, _binary_##NAME)

const unsigned char* fz_lookup_base14_font(fz_context*, const char* name, int* len) {
    if (!strcmp(name, "Courier")) RETURN(NimbusMonoPS_Regular_cff);
    if (!strcmp(name, "Courier-Oblique")) RETURN(NimbusMonoPS_Italic_cff);
    if (!strcmp(name, "Courier-Bold")) RETURN(NimbusMonoPS_Bold_cff);
    if (!strcmp(name, "Courier-BoldOblique")) RETURN(NimbusMonoPS_BoldItalic_cff);
    if (!strcmp(name, "Helvetica")) RETURN(NimbusSans_Regular_cff);
    if (!strcmp(name, "Helvetica-Oblique")) RETURN(NimbusSans_Italic_cff);
    if (!strcmp(name, "Helvetica-Bold")) RETURN(NimbusSans_Bold_cff);
    if (!strcmp(name, "Helvetica-BoldOblique")) RETURN(NimbusSans_BoldItalic_cff);
    if (!strcmp(name, "Times-Roman")) RETURN(NimbusRoman_Regular_cff);
    if (!strcmp(name, "Times-Italic")) RETURN(NimbusRoman_Italic_cff);
    if (!strcmp(name, "Times-Bold")) RETURN(NimbusRoman_Bold_cff);
    if (!strcmp(name, "Times-BoldItalic")) RETURN(NimbusRoman_BoldItalic_cff);
    if (!strcmp(name, "Symbol")) RETURN(StandardSymbolsPS_cff);
    if (!strcmp(name, "ZapfDingbats")) RETURN(Dingbats_cff);
    *len = 0;
    return nullptr;
}

const unsigned char* fz_lookup_builtin_font(fz_context*, const char* name, int bold, int italic, int* len) {
    if (!strcmp(name, "Courier")) {
        if (bold) { if (italic) RETURN(NimbusMonoPS_BoldItalic_cff); RETURN(NimbusMonoPS_Bold_cff); }
        if (italic) RETURN(NimbusMonoPS_Italic_cff);
        RETURN(NimbusMonoPS_Regular_cff);
    }
    if (!strcmp(name, "Helvetica") || !strcmp(name, "Arial")) {
        if (bold) { if (italic) RETURN(NimbusSans_BoldItalic_cff); RETURN(NimbusSans_Bold_cff); }
        if (italic) RETURN(NimbusSans_Italic_cff);
        RETURN(NimbusSans_Regular_cff);
    }
    if (!strcmp(name, "Times") || !strcmp(name, "Times Roman") || !strcmp(name, "Times New Roman")) {
        if (bold) { if (italic) RETURN(NimbusRoman_BoldItalic_cff); RETURN(NimbusRoman_Bold_cff); }
        if (italic) RETURN(NimbusRoman_Italic_cff);
        RETURN(NimbusRoman_Regular_cff);
    }
    if (!strcmp(name, "Dingbats") || !strcmp(name, "Zapf Dingbats")) RETURN(Dingbats_cff);
    if (!strcmp(name, "Symbol")) RETURN(StandardSymbolsPS_cff);
    *len = 0;
    return nullptr;
}

const unsigned char* fz_lookup_cjk_font(fz_context*, int, int* len, int* index) {
    *len = 0;
    if (index) *index = 0;
    return nullptr;
}

const unsigned char* fz_lookup_cjk_font_by_language(fz_context*, const char*, int* len, int* subfont) {
    *len = 0;
    if (subfont) *subfont = 0;
    return nullptr;
}

int fz_lookup_cjk_ordering_by_language(const char*) { return -1; }

const unsigned char* fz_lookup_noto_font(fz_context*, int, int, int* len, int* subfont) {
    *len = 0;
    if (subfont) *subfont = 0;
    return nullptr;
}

const unsigned char* fz_lookup_noto_math_font(fz_context*, int* len) { *len = 0; return nullptr; }
const unsigned char* fz_lookup_noto_music_font(fz_context*, int* len) { *len = 0; return nullptr; }
const unsigned char* fz_lookup_noto_symbol1_font(fz_context*, int* len) { *len = 0; return nullptr; }
const unsigned char* fz_lookup_noto_symbol2_font(fz_context*, int* len) { *len = 0; return nullptr; }
const unsigned char* fz_lookup_noto_emoji_font(fz_context*, int* len) { *len = 0; return nullptr; }
