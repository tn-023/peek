#include "core/sheetlayout.h"
#include "core/atlaslayout.h"
// Einfache Tests fuer den plattformunabhaengigen Kern (ohne Test-Framework).
#include "core/patterns.h"
#include "core/lang.h"
#include "core/query.h"
#include "core/textutil.h"
#include "core/svgcheck.h"
#include "core/tga.h"
#include "core/heif.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace peek;

static int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FEHLER %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)

static bool Has(const std::vector<std::wstring>& v, const wchar_t* s) {
    for (auto& x : v) if (x == s) return true;
    return false;
}

static void TestTokenizer() {
    auto t = TokenizeName(L"SommerUrlaub_Rom-2024 (12)");
    CHECK(Has(t, L"sommer"));
    CHECK(Has(t, L"urlaub"));
    CHECK(Has(t, L"rom"));
    CHECK(!Has(t, L"2024"));
    CHECK(!Has(t, L"12"));

    auto u = TokenizeName(L"München_Straße");
    CHECK(Has(u, L"muenchen"));
    CHECK(Has(u, L"strasse"));

    auto v = TokenizeName(L"img2041_HDRImage");
    CHECK(!Has(v, L"img"));     // Stoppwort
    CHECK(Has(v, L"hdr"));
    CHECK(Has(v, L"image"));
}

static void TestTemplates() {
    CHECK(NameTemplate(L"IMG_20240817_1430") == L"IMG_{Datum}_####");
    CHECK(NameTemplate(L"Urlaub (12)") == L"Urlaub (##)");
    CHECK(NameTemplate(L"Screenshot 2024-08-17 101500") == L"Screenshot {Datum} ######");
    CHECK(DateFromName(L"PXL_20230105_123456") == DaysFromCivil(2023, 1, 5));
    CHECK(DateFromName(L"foto") == -1);
    CHECK(DetectSource(L"Screenshot 2024") == L"Bildschirmfoto");
    CHECK(DetectSource(L"DSC01234") == L"Kamera (DSC)");
}

static void TestFacets() {
    TokenDict dict;
    std::vector<std::vector<uint32_t>> docs;
    const wchar_t* names[] = {L"rom_urlaub_1", L"rom_urlaub_2", L"rom_kolosseum", L"paris_urlaub",
                              L"berlin_x", L"berlin_y", L"berlin_z", L"wien"};
    for (auto n : names) {
        std::vector<uint32_t> ids;
        for (auto& w : TokenizeName(n)) ids.push_back(dict.Id(w));
        docs.push_back(ids);
    }
    std::vector<const std::vector<uint32_t>*> ptrs;
    for (auto& d : docs) ptrs.push_back(&d);
    auto f = ComputeFacets(ptrs, 10, 3, 0.8);
    CHECK(f.size() == 3);  // rom, urlaub, berlin
    bool hasRom = false;
    for (auto& x : f) if (dict.Word(x.id) == L"rom" && x.count == 3) hasRom = true;
    CHECK(hasRom);
}

static MatchInput Input(const wchar_t* name, uint32_t w, uint32_t h, uint64_t size, int64_t time) {
    MatchInput in;
    in.name = name;
    in.ext = L"jpg";
    in.kind = Kind::Image;
    in.width = w;
    in.height = h;
    in.size = size;
    in.time = time;
    return in;
}

static void TestLang() {
    CHECK(CurrentLang() == Lang::De);
    CHECK(std::wstring(T(L"Ordner")) == L"Ordner");
    CHECK(FormatThousands(1234567) == L"1.234.567");
    CHECK(FormatDay(2024, 8, 17) == L"17.08.2024");
    CHECK(FormatSize(1536 * 1024) == L"1,5 MB");
    CHECK(std::wstring(QueryParams().front().label) == L"Breite");
    CHECK(TPattern(L"IMG_{Datum}_####") == L"IMG_{Datum}_####");
    SetCurrentLang(Lang::En);
    CHECK(std::wstring(T(L"Ordner")) == L"Folder");
    CHECK(T(std::wstring(L"Ordner")) == L"Folder");
    CHECK(std::wstring(T(L"gibt es nicht")) == L"gibt es nicht");   // unbekannt: bleibt deutsch
    CHECK(std::wstring(TC(L"sidebar:Bilder", L"Bilder")) == L"Pictures");
    CHECK(TF(L"Seite {0} von {1}", {L"2", L"5"}) == L"Page 2 of 5");
    CHECK(FormatThousands(1234567) == L"1,234,567");
    CHECK(FormatDay(2024, 8, 17) == L"17 Aug 2024");
    CHECK(FormatSize(1536 * 1024) == L"1.5 MB");
    CHECK(FormatDayTime(2024, 8, 17, 9, 5) == L"17 Aug 2024, 09:05");
    CHECK(std::wstring(MonthName(3)) == L"March");
    CHECK(std::wstring(QueryParams().front().label) == L"Width");
    CHECK(TPattern(L"IMG_{Datum}_####") == L"IMG_{date}_####");
    CHECK(EnglishEntryCount() > 100);
    SetCurrentLang(Lang::De);
    CHECK(std::wstring(MonthName(3)) == L"März");
}

static void TestQuery() {
    double b = 0;
    CHECK(ParseSize(L"10mb", b) && b == 10.0 * 1024 * 1024);
    CHECK(ParseSize(L"1,5gb", b) && b == 1.5 * 1024 * 1024 * 1024);
    double s = 0;
    CHECK(ParseDuration(L"2m", s) && s == 120);

    int64_t from = 0, to = 0;
    CHECK(ParseDateSpan(L"2024-08", from, to));
    CHECK(from == DaysFromCivil(2024, 8, 1) * 86400);
    CHECK(to == DaysFromCivil(2024, 9, 1) * 86400);

    int64_t aug = DaysFromCivil(2024, 8, 17) * 86400;
    int64_t jan = DaysFromCivil(2023, 1, 2) * 86400;

    auto q = ParseQuery(L"rom w:>3000 size:>1mb date:2024 -screenshot");
    CHECK(q.terms.size() == 1 && q.terms[0] == L"rom");
    CHECK(Matches(q, Input(L"rom_urlaub.jpg", 4000, 3000, 5 * 1024 * 1024, aug)));
    CHECK(!Matches(q, Input(L"rom_urlaub.jpg", 2000, 1500, 5 * 1024 * 1024, aug)));
    CHECK(!Matches(q, Input(L"rom_urlaub.jpg", 4000, 3000, 5 * 1024 * 1024, jan)));
    CHECK(!Matches(q, Input(L"rom_screenshot.jpg", 4000, 3000, 5 * 1024 * 1024, aug)));

    auto r = ParseQuery(L"date:2024-08..2024-09 ext:jpg,png urlaub*");
    CHECK(Matches(r, Input(L"urlaub_1.jpg", 1, 1, 1, aug)));
    CHECK(!Matches(r, Input(L"rom_urlaub.jpg", 1, 1, 1, aug)));

    auto t = ParseQuery(L"type:video");
    CHECK(!Matches(t, Input(L"a.jpg", 1, 1, 1, aug)));

    {
        auto g = ParseQuery(L"Breite:>3000 H\u00f6he:<100 Gr\u00f6\u00dfe:>1mb Aufgenommen:2024 Endung:jpg Megapixel:>=20");
        CHECK(g.width.op == NumCond::Gt && g.width.a == 3000);
        CHECK(g.height.op == NumCond::Lt);
        CHECK(g.size.active());
        CHECK(g.taken.active());
        CHECK(g.megapixel.active());
        CHECK(g.exts.size() == 1 && g.exts[0] == L"jpg");
        CHECK(g.terms.empty());
    }
    {
        // englische Schreibweisen gelten immer, unabhaengig von der Sprache der Oberflaeche
        auto g = ParseQuery(L"Width:>3000 Height:<100 Size:>1mb Captured:2024 Extension:jpg Megapixels:>=20 Duration:>1m "
                            L"Camera:x-t5 type:images");
        CHECK(g.width.op == NumCond::Gt && g.width.a == 3000);
        CHECK(g.height.op == NumCond::Lt);
        CHECK(g.size.active() && g.taken.active() && g.megapixel.active() && g.durationSec.active());
        CHECK(g.exts.size() == 1 && g.exts[0] == L"jpg");
        CHECK(g.cams.size() == 1);
        CHECK(g.type == static_cast<int>(Kind::Image));
        CHECK(g.terms.empty());
        auto t2 = ParseQuery(L"taken:2024 date:2024");
        CHECK(t2.taken.active() && t2.date.active());
    }
    {
        SheetOptions so;
        auto g = ComputeSheet(so);
        CHECK(g.PerPage() == 20);
        CHECK(g.cells[0].x > 30 && g.cells[0].y > g.header.Bottom());
        CHECK(g.cells.back().Right() <= g.pageW - kSheetMargin + 0.01f);
        CHECK(g.captions.back().Bottom() <= g.footer.y);
        so.landscape = true;
        auto l = ComputeSheet(so);
        CHECK(l.pageW > l.pageH);
        CHECK(SheetPageCount(0, 20) == 1 && SheetPageCount(21, 20) == 2);
        auto w = [](const std::wstring& x) { return static_cast<float>(x.size()); };
        CHECK(MiddleEllipsis(L"IMG_20240817_123456.jpg", 12, w).size() <= 12);
        CHECK(MiddleEllipsis(L"kurz.jpg", 12, w) == L"kurz.jpg");
        auto r = FitInto({0, 0, 100, 100}, 200, 100);
        CHECK(r.w == 100 && r.h == 50 && r.y == 25);
    }
    auto e = ParseQuery(L"");
    CHECK(e.Empty());
}

static void TestText() {
    CHECK(ToLower(L"ÄBC") == L"äbc");
    CHECK(WildcardMatch(L"img_*.jpg", L"img_0001.jpg"));
    CHECK(!WildcardMatch(L"img_?.jpg", L"img_10.jpg"));
    CHECK(FormatDuration(65000) == L"1:05");
    int y; unsigned m, d;
    CivilFromDays(DaysFromCivil(2026, 11, 12), y, m, d);
    CHECK(y == 2026 && m == 11 && d == 12);
}

static void TestAtlas() {
    CHECK(AtlasRows(10, 4) == 3);
    CHECK(AtlasRows(1, 5) == 1);
    CHECK(AtlasAutoCols(16, 64, 64) == 4);
    CHECK(AtlasAutoCols(32, 512, 512) == 6);          // 6 x 6 = 3072 x 3072 (4 leer)
    CHECK(AtlasAutoCols(8, 100, 50) == 2);            // 200 x 200
    CHECK(AtlasAutoCols(1, 10, 10) == 1);
    CHECK(AtlasAutoCols(12, 64, 64) == 4);            // 4 x 3 (256 x 192) statt 3 x 4
    int c = AtlasColsForMoreRows(16, 4);
    CHECK(AtlasRows(16, c) == 6 && c == 3);   // 4 Zeilen -> naechste moegliche Aufteilung
    c = AtlasColsForFewerRows(16, 4);
    CHECK(AtlasRows(16, c) == 3 && c == 6);
    CHECK(AtlasColsForMoreRows(5, 1) == 1);
    CHECK(AtlasColsForFewerRows(5, 5) == 5);
}

static void TestSvgCheck() {
    // normale Dateien
    CHECK(SvgEntitiesSafe("<svg xmlns='http://www.w3.org/2000/svg'><image href='a.png'/></svg>"));
    CHECK(SvgEntitiesSafe("<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" "
                          "\"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\"><svg/>"));
    // Illustrator: Namespace-Abkuerzungen
    CHECK(SvgEntitiesSafe("<!DOCTYPE svg [\n\t<!ENTITY ns_svg \"http://www.w3.org/2000/svg\">\n"
                          "\t<!ENTITY ns_xlink 'http://www.w3.org/1999/xlink'>\n]><svg xmlns=\"&ns_svg;\"/>"));
    // Markup in einer Entity (versteckter Verweis)
    CHECK(!SvgEntitiesSafe("<!DOCTYPE svg [<!ENTITY e \"<image href='\\\\server\\x.png'/>\">]><svg>&e;</svg>"));
    // Markup ueber Zeichenreferenzen
    CHECK(!SvgEntitiesSafe("<!DOCTYPE svg [<!ENTITY e \"&#60;image &#104;ref='x'/&#62;\">]><svg>&e;</svg>"));
    // verschachtelte Entity, Parameter-Entity, externe Entity
    CHECK(!SvgEntitiesSafe("<!DOCTYPE svg [<!ENTITY a 'x'><!ENTITY b '&a;&a;'>]><svg/>"));
    CHECK(!SvgEntitiesSafe("<!DOCTYPE svg [<!ENTITY % p 'x'>]><svg/>"));
    CHECK(!SvgEntitiesSafe("<!DOCTYPE svg [<!ENTITY e SYSTEM 'file:///c:/x'>]><svg>&e;</svg>"));
    CHECK(!SvgEntitiesSafe("<!DOCTYPE svg [<!ENTITY e PUBLIC 'x' '\\\\server\\x'>]><svg>&e;</svg>"));
    // kaputt
    CHECK(!SvgEntitiesSafe("<!DOCTYPE svg [<!ENTITY e \"offen"));
    CHECK(!SvgEntitiesSafe("<!ENTITYx 'a'>"));
}

static std::vector<uint8_t> TgaHdr(uint8_t type,uint16_t w,uint16_t h,uint8_t bits,uint8_t desc,uint8_t mapType=0,uint16_t mapLen=0,uint8_t mapBits=0){
  std::vector<uint8_t> d(18,0); d[1]=mapType; d[2]=type; d[5]=mapLen&255; d[6]=mapLen>>8; d[7]=mapBits;
  d[12]=w&255; d[13]=w>>8; d[14]=h&255; d[15]=h>>8; d[16]=bits; d[17]=desc; return d; }
static void TestTga() {
  std::vector<uint8_t> out; uint32_t w,h;
  // 2x2 24 bit unkomprimiert, von unten: Zeile unten zuerst
  auto d=TgaHdr(2,2,2,24,0); uint8_t px[]={1,2,3, 4,5,6, 7,8,9, 10,11,12}; d.insert(d.end(),px,px+12);
  CHECK(TgaDecode(d.data(),d.size(),1<<20,out,w,h) && w==2&&h==2);
  CHECK(out[0]==7&&out[1]==8&&out[2]==9&&out[3]==255); // oben links = 3. Pixel der Datei
  CHECK(out[12]==4 && out[15]==255);
  // abgeschnitten
  CHECK(!TgaDecode(d.data(),d.size()-1,1<<20,out,w,h));
  // RLE 32 bit mit alpha, von oben
  auto r=TgaHdr(10,3,1,32,0x28); uint8_t rp[]={0x82, 10,20,30,128}; r.insert(r.end(),rp,rp+5);
  CHECK(TgaDecode(r.data(),r.size(),1<<20,out,w,h) && w==3 && out[3]==128 && out[0]==5 && out[8+3]==128);
  // RLE ueber Bildende
  auto r2=TgaHdr(10,2,1,32,0x28); uint8_t rp2[]={0x83, 1,2,3,4}; r2.insert(r2.end(),rp2,rp2+5);
  CHECK(!TgaDecode(r2.data(),r2.size(),1<<20,out,w,h));
  // 32 bit ohne Alphaangabe: deckend
  auto o=TgaHdr(2,1,1,32,0x20); uint8_t op[]={9,9,9,0}; o.insert(o.end(),op,op+4);
  CHECK(TgaDecode(o.data(),o.size(),1<<20,out,w,h) && out[3]==255 && out[0]==9);
  // Palette: 2 Eintraege 24 Bit, Index 1
  auto m=TgaHdr(1,1,1,8,0x20,1,2,24); uint8_t mp[]={1,1,1, 50,60,70, 1}; m.insert(m.end(),mp,mp+7);
  CHECK(TgaDecode(m.data(),m.size(),1<<20,out,w,h) && out[0]==50 && out[2]==70);
  auto m2=m; m2.back()=5; CHECK(!TgaDecode(m2.data(),m2.size(),1<<20,out,w,h)); // Index ausserhalb
  // Graustufen RLE
  auto g=TgaHdr(11,4,1,8,0x20); uint8_t gp[]={0x01,7,8, 0x81,9}; g.insert(g.end(),gp,gp+5);
  CHECK(TgaDecode(g.data(),g.size(),1<<20,out,w,h) && out[0]==7 && out[4]==8 && out[8]==9 && out[12]==9);
  // 16 bit: weiss mit Alphabit
  auto s=TgaHdr(2,1,1,16,0x21); uint8_t sp[]={0xFF,0xFF}; s.insert(s.end(),sp,sp+2);
  CHECK(TgaDecode(s.data(),s.size(),1<<20,out,w,h) && out[0]==255 && out[3]==255);
  // zu gross
  auto big=TgaHdr(2,60000,60000,24,0); CHECK(!TgaDecode(big.data(),big.size(),1<<20,out,w,h));
  // ungueltige Typen/Tiefen
  auto bad=TgaHdr(2,1,1,12,0); bad.resize(30); CHECK(!TgaDecode(bad.data(),bad.size(),1<<20,out,w,h));
  auto bad2=TgaHdr(4,1,1,24,0); bad2.resize(30); TgaInfo ti; CHECK(!TgaReadInfo(bad2.data(),bad2.size(),ti));
  CHECK(TgaReadInfo(d.data(),d.size(),ti) && ti.width==2);
  // von rechts
  auto rr=TgaHdr(2,2,1,24,0x30); uint8_t rrp[]={1,1,1, 2,2,2}; rr.insert(rr.end(),rrp,rrp+6);
  CHECK(TgaDecode(rr.data(),rr.size(),1<<20,out,w,h) && out[0]==2 && out[4]==1);
  // Zufallsdaten: darf nicht abstuerzen
  unsigned seed=1; for(int k=0;k<200000;k++){ std::vector<uint8_t> z(18+(seed%200)); for(auto&c:z){seed=seed*1103515245+12345; c=seed>>16;} z[2]=(seed%2)?10:2; z[16]=32; z[12]=z[12]%16+1; z[13]=0; z[14]=z[14]%16+1; z[15]=0; TgaDecode(z.data(),z.size(),1<<20,out,w,h);} 
}


// --- HEIF-Kopf und EXIF ---------------------------------------------------
static void Be32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(uint8_t(x >> (i * 8))); }
static void Be16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
static void Le16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
static void Le32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (i * 8))); }
static std::vector<uint8_t> Bx(const char* t, const std::vector<uint8_t>& p, int ver = -1, uint32_t flags = 0) {
    std::vector<uint8_t> v;
    Be32(v, uint32_t(8 + (ver >= 0 ? 4 : 0) + p.size()));
    v.insert(v.end(), t, t + 4);
    if (ver >= 0) { v.push_back(uint8_t(ver)); v.push_back(uint8_t(flags >> 16)); v.push_back(uint8_t(flags >> 8)); v.push_back(uint8_t(flags)); }
    v.insert(v.end(), p.begin(), p.end());
    return v;
}
static void Cat(std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { a.insert(a.end(), b.begin(), b.end()); }

// kleiner TIFF-Block (Little Endian): Hersteller, Modell, EXIF-Verzeichnis mit Datum, Blende, Belichtung, ISO, Objektiv
static std::vector<uint8_t> TestTiff() {
    const char make[] = "Apple", model[] = "iPhone 15 Pro", date[] = "2024:08:17 14:30:05", lens[] = "iPhone 15 Pro back camera";
    // Aufbau: Kopf(8) | IFD0 (2+3*12+4=42) | EXIF-IFD (2+5*12+4=66) | Daten
    const uint32_t ifd0 = 8, exif = ifd0 + 42, data0 = exif + 66;
    std::vector<uint8_t> dat;
    auto add = [&](const void* p, size_t n) { uint32_t at = data0 + uint32_t(dat.size()); dat.insert(dat.end(), (const uint8_t*)p, (const uint8_t*)p + n); return at; };
    uint32_t oMake = add(make, sizeof make), oModel = add(model, sizeof model), oDate = add(date, sizeof date), oLens = add(lens, sizeof lens);
    std::vector<uint8_t> r1, r2;
    Le32(r1, 178); Le32(r1, 100);
    Le32(r2, 1); Le32(r2, 250);
    uint32_t oF = add(r1.data(), 8), oE = add(r2.data(), 8);
    std::vector<uint8_t> v = {'I', 'I'};
    Le16(v, 42); Le32(v, ifd0);
    auto ent = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t val) { Le16(v, tag); Le16(v, type); Le32(v, count); Le32(v, val); };
    Le16(v, 3);
    ent(271, 2, sizeof make, oMake);
    ent(272, 2, sizeof model, oModel);
    ent(34665, 4, 1, exif);
    Le32(v, 0);
    Le16(v, 5);
    ent(33434, 5, 1, oE);
    ent(33437, 5, 1, oF);
    ent(34855, 3, 1, 125);
    ent(36867, 2, sizeof date, oDate);
    ent(42036, 2, sizeof lens, oLens);
    Le32(v, 0);
    Cat(v, dat);
    return v;
}

static std::vector<uint8_t> TestHeif(uint32_t exifOffset, size_t exifLen) {
    std::vector<uint8_t> f, ftyp;
    const char brands[] = "heic\0\0\0\0mif1heic";
    ftyp.insert(ftyp.end(), brands, brands + 16);
    Cat(f, Bx("ftyp", ftyp));
    std::vector<uint8_t> meta, p;
    Cat(meta, Bx("hdlr", std::vector<uint8_t>(20, 0), 0));
    p.clear(); Be16(p, 1); Cat(meta, Bx("pitm", p, 0));
    std::vector<uint8_t> iinf, e1, e2;
    Be16(iinf, 2);
    Be16(e1, 1); Be16(e1, 0); e1.insert(e1.end(), {'h', 'v', 'c', '1', 0});
    Be16(e2, 2); Be16(e2, 0); e2.insert(e2.end(), {'E', 'x', 'i', 'f', 0});
    Cat(iinf, Bx("infe", e1, 2));
    Cat(iinf, Bx("infe", e2, 2));
    Cat(meta, Bx("iinf", iinf, 0));
    std::vector<uint8_t> iloc = {0x44, 0x00};
    Be16(iloc, 1);
    Be16(iloc, 2); Be16(iloc, 0); Be16(iloc, 1); Be32(iloc, exifOffset); Be32(iloc, uint32_t(exifLen));
    Cat(meta, Bx("iloc", iloc, 0));
    std::vector<uint8_t> ipco, ispe, irot = {3}, ipma;
    Be32(ispe, 4032); Be32(ispe, 3024);
    Cat(ipco, Bx("ispe", ispe, 0));
    Cat(ipco, Bx("irot", irot));
    Be32(ipma, 1);
    Be16(ipma, 1); ipma.push_back(2); ipma.push_back(0x01); ipma.push_back(0x82);
    std::vector<uint8_t> iprp = Bx("ipco", ipco);
    Cat(iprp, Bx("ipma", ipma, 0));
    Cat(meta, Bx("iprp", iprp));
    Cat(f, Bx("meta", meta, 0));
    return f;
}

static void TestHeif() {
    CHECK(HeifOrientation(0, -1, false) == 1);
    CHECK(HeifOrientation(1, -1, false) == 8);
    CHECK(HeifOrientation(2, -1, false) == 3);
    CHECK(HeifOrientation(3, -1, false) == 6);
    CHECK(HeifOrientation(0, 1, false) == 2);
    CHECK(HeifOrientation(0, 0, false) == 4);
    CHECK(HeifOrientation(1, 0, false) == 5);   // wie libavif: irot 1 + imir 0
    CHECK(HeifOrientation(3, 0, false) == 7);

    std::vector<uint8_t> tiff = TestTiff();
    std::vector<uint8_t> item;
    Be32(item, 6);
    item.insert(item.end(), {'E', 'x', 'i', 'f', 0, 0});
    Cat(item, tiff);
    size_t headLen = TestHeif(0, 0).size();
    std::vector<uint8_t> file = TestHeif(uint32_t(headLen + 8), item.size());
    std::vector<uint8_t> mdat = Bx("mdat", item);
    Cat(file, mdat);

    HeifInfo hi;
    CHECK(HeifParse(file.data(), file.size(), hi));
    CHECK(hi.width == 4032 && hi.height == 3024);
    CHECK(hi.orientation == 6);
    CHECK(hi.exifOffset == headLen + 8 && hi.exifLength == item.size());
    CHECK(hi.metaEnd <= headLen);

    ExifInfo ex;
    CHECK(HeifExifParse(file.data() + hi.exifOffset, size_t(hi.exifLength), ex));
    CHECK(ex.make == "Apple" && ex.model == "iPhone 15 Pro");
    CHECK(ex.dateTaken == "2024:08:17 14:30:05");
    CHECK(ex.fnumber > 1.779 && ex.fnumber < 1.781);
    CHECK(ex.exposure > 0.00399 && ex.exposure < 0.00401);
    CHECK(ex.iso == 125);
    CHECK(ex.lens == "iPhone 15 Pro back camera");

    // abgeschnittener Kopf: meldet die benoetigte Laenge
    HeifInfo cut;
    CHECK(!HeifParse(file.data(), 200, cut));
    CHECK(cut.metaEnd == headLen);
    CHECK(!HeifParse(tiff.data(), tiff.size(), cut));   // kein HEIF

    // Zufallstest: veraenderte und gekuerzte Dateien duerfen nichts ausser false/true liefern
    uint32_t seed = 2024;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int i = 0; i < 20000; ++i) {
        std::vector<uint8_t> m = file;
        int changes = 1 + int(rnd() % 8);
        for (int k = 0; k < changes; ++k) m[rnd() % m.size()] = uint8_t(rnd());
        size_t n = rnd() % (m.size() + 1);
        HeifInfo h2;
        if (HeifParse(m.data(), n, h2) && h2.exifLength && h2.exifOffset < n) {
            ExifInfo e2;
            HeifExifParse(m.data() + h2.exifOffset, size_t(std::min<uint64_t>(h2.exifLength, n - h2.exifOffset)), e2);
        }
        std::vector<uint8_t> t = tiff;
        for (int k = 0; k < changes; ++k) t[rnd() % t.size()] = uint8_t(rnd());
        ExifInfo e3;
        ExifParse(t.data(), rnd() % (t.size() + 1), e3);
    }
}

int main() {
    TestHeif();
    TestAtlas();
    TestText();
    TestTokenizer();
    TestTemplates();
    TestFacets();
    TestQuery();
    TestLang();
    TestSvgCheck();
    TestTga();
    if (g_failed) {
        std::printf("%d Test(s) fehlgeschlagen\n", g_failed);
        return 1;
    }
    std::printf("Alle Tests bestanden\n");
    return 0;
}
