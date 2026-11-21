/* Host check of the page service: filter a file, or run the built-in cases. */
#include "net_page.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t out[NET_PAGE_OUT_MAX + 1U];

static const char *run(const char *html, const char *type, const char *base)
{
    int cut = 0;
    size_t n = net_page_filter((const uint8_t *)html, strlen(html), type, base, NULL, 0, out, NET_PAGE_OUT_MAX, &cut);
    assert(!cut && n <= NET_PAGE_OUT_MAX);
    out[n] = 0;
    for (size_t i = 0; i < n; ++i) assert(out[i] >= 9 && out[i] < 127);
    return (const char *)out;
}
static void expect(const char *html, const char *want)
{
    const char *got = run(html, "text/html; charset=utf-8", "https://site.test/dir/page.html?x=1");
    if (strcmp(got, want)) {
        printf("input: %s\n want: %s\n  got: %s\n", html, want, got);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 4) {
        static uint8_t data[NET_PAGE_RAW_MAX];
        FILE *f = fopen(argv[1], "rb");
        int cut = 0;
        size_t n, m;
        if (!f) return 2;
        n = fread(data, 1, sizeof(data), f);
        fclose(f);
        {
            static uint8_t css[NET_PAGE_CSS_MAX];
            size_t css_length = 0;
            if (argc >= 5 && (f = fopen(argv[4], "rb")) != NULL) { css_length = fread(css, 1, sizeof(css), f); fclose(f); }
            m = net_page_filter(data, n, "text/html", argv[3], css, css_length, out, NET_PAGE_OUT_MAX, &cut);
        }
        printf("in=%u out=%u cut=%d\n", (unsigned)n, (unsigned)m, cut);
        f = fopen(argv[2], "wb");
        if (!f) return 2;
        fwrite(out, 1, m, f);
        fclose(f);
        return 0;
    }
    assert(net_page_is_html("text/html") && net_page_is_html("Text/HTML; charset=x") &&
           net_page_is_html("application/xhtml+xml") && !net_page_is_html("text/plain") &&
           !net_page_is_html(""));

    /* Head, scripts, styles and comments go; a head without an end tag and
     * quotes or "</" inside a script do not hide the page. */
    expect("<html><head><title>T</title><meta charset=utf-8><style>p{a:'</p>'}</style>"
           "<script>var s='</div>\"';if(a<b){}</script><body><p>Hello <!-- no --> world</p>",
           "<p>Hello world<br>");
    expect("<head><script>x='<p>no</p>'</script>Text after head", "Text after head");
    /* Blocks, headings, lists, space collapse. */
    expect("<h1> Title </h1><div><div>a   b\n c</div></div><ul><li>one<li>two</ul>",
           "<h1>Title</h1>a b c<br><li>one<li>two<br>");
    /* Entities and UTF-8 become ASCII; < and & stay escaped. */
    expect("caf\xC3\xA9 &eacute;t&eacute; &mdash; &#8220;q&#x201D; 1&lt;2 &amp; &copy; \xE2\x80\xA6 &unknown; a&b",
           "cafe ete -- \"q\" 1&lt;2 &amp; (c) ... ? a&amp;b");
    /* Links: kept with their URL, dead schemes dropped, download kept. */
    expect("<a href='/x?a=1&amp;b=2'>go</a> <a href=\"javascript:void(0)\">js</a> "
           "<a href=f.zip download=\"n.zip\">get</a><a href=#top>up</a>",
           "<a href=\"/x?a=1&b=2\">go</a> js <a href=\"f.zip\" download=\"n.zip\">get</a><a href=\"#top\">up</a>");
    /* Only the targets of same-page links keep their id. */
    expect("<a href='#sec2'>jump</a><h2 id=sec1>A</h2><h2 id=sec2>B</h2><a name=sec2></a>",
           "<a href=\"#sec2\">jump</a><h2>A</h2><b id=\"sec2\"><h2>B</h2><b id=\"sec2\">");
    /* Images: lazy-load attribute, srcset, tracking pixel, SVG, alt text. */
    expect("<img src='data:image/gif;base64,AAAA' data-src='/real.jpg'>"
           "<img srcset='/a-320.jpg 320w, /a-640.jpg 640w'>"
           "<img src=/pixel.gif width=1 height=1><img src=/logo.svg alt='Logo'>",
           "<img src=\"/real.jpg\"><img src=\"/a-320.jpg\">[Logo]");
    /* Images stay inline and carry the display size in pixels. */
    expect("<p>See <img src=a.png width=120 height=\"40px\"> here <img src=b.png width=50% height=030>.",
           "<p>See <img src=\"a.png\" w=120 h=40> here <img src=\"b.png\">.");
    /* CSS sizes: a linked sheet, a <style> block with an ancestor part and
     * a media rule, and a style attribute; the heaviest rule wins. */
    {
        const char *sheet = "/* c */ img{max-width:100%} .logo{width:10rem;height:40px} @media print{.logo{width:1px}}";
        const char *html = "<link rel=stylesheet href='/s/a.css'><link rel=icon href=i.png>"
            "<style>.hero img.big{width:300px} @media (max-width:400px){.hero img.big{width:50px}}"
            " #x{height:2em} p:hover img{width:9px}</style>"
            "<img class='logo' src=a.png><div class=hero><img class='big wide' id=x src=b.png width=20></div>"
            "<img src=c.png style='width: 77px; height:auto'>";
        char url[128];
        int cut = 0;
        size_t n = net_page_filter((const uint8_t *)html, strlen(html), "text/html", "https://a.test/d/p.html",
                                   (const uint8_t *)sheet, strlen(sheet), out, NET_PAGE_OUT_MAX, &cut);
        out[n] = 0;
        assert(!strcmp((const char *)out, "<img src=\"a.png\" w=160 h=40><br><img src=\"b.png\" w=300 h=32><br>"
                                         "<img src=\"c.png\" w=77>"));
        assert(net_page_stylesheet((const uint8_t *)html, strlen(html), "https://a.test/d/p.html", 0, url, sizeof(url)) &&
               !strcmp(url, "https://a.test/s/a.css"));
        assert(!net_page_stylesheet((const uint8_t *)html, strlen(html), "https://a.test/d/p.html", 1, url, sizeof(url)));
    }
    /* Hidden and unsupported elements go, with nested elements. */
    expect("a<div hidden>x<div>y</div>z</div>b<span style='display: none'>c</span>"
           "<svg><path d=''/></svg><noscript>d</noscript><div aria-hidden=true>e</div>f",
           "abdf");
    /* GET form: the action loses its query; hidden and typed fields. */
    expect("<form action='/search?old=1'><input type=hidden name=hl value='en US'>"
           "<input name=q><input type=submit value=Go></form>",
           "<a href=\"https://site.test/search?hl=en+US&q=%s\">[Form: q]</a><br>");
    /* POST form with select, checkbox, textarea and a relative action. */
    expect("<form method=POST action=send.php><select name=s><option value=1>One"
           "<option value=2 selected>Two</select><input type=checkbox name=c checked>"
           "<input type=checkbox name=d><textarea name=msg>old</textarea>"
           "<input type=password name=pw></form>",
           "<a href=\"post:https://site.test/dir/send.php s=2&c=on&msg=%s&pw=%s\">[Form: msg, pw]</a><br>");
    /* A form with no action posts to the page; no fields means no link. */
    expect("<form><input type=hidden name=a value=b></form><form></form>x",
           "<a href=\"https://site.test/dir/page.html?a=b\">[Send form]</a><br>x");
    /* Latin-1 input. */
    assert(!strcmp(run("caf\xE9 \xA9", "text/html; charset=ISO-8859-1", "https://a.test/"), "cafe (c)"));
    /* An unfinished tag or entity at the end does not read past the input. */
    expect("text <a href=\"x", "text <a href=\"x\"></a>");
    expect("a &#", "a &amp;#");
    expect("a <", "a &lt;");

    /* Output limit: the result fits, ends with the note, and has no half tag. */
    {
        static char big[400000];
        int cut = 0;
        size_t used = 0, n;
        while (used + 64 < sizeof(big))
            used += (size_t)sprintf(big + used, "<p>Line <a href=\"/link%u\">link</a> text", (unsigned)used);
        n = net_page_filter((const uint8_t *)big, used, "text/html", "https://a.test/", NULL, 0, out, NET_PAGE_OUT_MAX, &cut);
        assert(cut && n <= NET_PAGE_OUT_MAX && n > 60000U);
        out[n] = 0;
        assert(!strcmp((const char *)out + n - 23, "<p>[Page cut: too long]"));
        assert(out[n - 24] == '>' || out[n - 24] != '<');
        assert(!strstr((const char *)out + n - 40, "<a hr") || strstr((const char *)out + n - 40, "\">"));
    }
    puts("PASS page service: head and scripts, blocks, ASCII text, links, anchors, images, hidden parts, forms, limits");
    return 0;
}
