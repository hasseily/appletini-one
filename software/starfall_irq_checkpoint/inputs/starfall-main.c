#include "libtini.h"
#include "game.h"
#include "starfall_assets.h"
#include "starfall_speech.h"
#include "starfall_music.h"
#include "starfall_bosses.h"
#include <string.h>
/* The sprite cells, speech phrases and song above stay in low RODATA, $0800-$1FFF.
 * This file's own tables and strings follow its code in main MAIN unless a
 * pragma below places them low. */
#pragma rodata-name (push, "CODE")

/* The simulation state stays near. The only full canvas is an owned banked
 * resource. An eight-line near strip composes sprites before normal CPU
 * publication. No mapped bank pointers, framebuffer acceleration or
 * firmware additions. */
#define G sf_state
uint8_t sf_mode, sf_lag;
uint32_t sf_frames, sf_steps, sf_tick, sf_score;
int16_t sf_player_x, sf_player_y;
uint8_t sf_hull, sf_weapon, sf_enemy_count, sf_bullet_count;
uint8_t sf_enemy_types_seen, sf_enemy_peak, sf_bullet_peak;
uint8_t sf_caption, sf_sector, sf_boss_number;
/* Joystick telemetry: 0 off, 1 answering, 2 silent and retried later,
 * and the last raw reading. */
uint8_t sf_stick;
ti_joystick sf_joystick;
static ti_surface canvas;
static uint8_t audio_ready, key, direction, tapped, bomb, last_buttons;
/* The cheat: letters of HELLO typed, the sector it chose, and the sector the
 * next step must start at. */
static uint8_t hello, warp, warp_due;
/* Phasor: music on voices 0-7, effects 8-11. Mockingboard: music 0-1, effects 2-5. */
static uint8_t music_voices;
static uint8_t stick_enabled=1, stick_shown;
/* The parallax backdrop of distant aircraft, chosen on the title screen. */
static uint8_t backdrop=1;
static uint32_t best_score;
static uint32_t caption_until, over_at;
/* Indices in the hash-pinned shared art palette. No archive art uses 14 or
 * 15; they are animated each drawn frame for shields and aimed bullets. */
#define dim 1
#define grey 2
#define white 4
#define cyan 5
#define red 7
#define amber 9
#define blue 13
#define shimmer 14
#define pulse 15

/* A line of interface text. */
typedef struct line { uint8_t x,y,color; const char *s; } line;
/* Stars, the backdrop, all entity pools and ship parts. Effects are added
 * last, so a full list drops explosion frames before anything that matters
 * to play. The playfield is 240 pixels, 120 bytes, wide. */
#define BACKDROP 24
#define ITEMS 112
/* In its own segment: until the scene starts, it is the file buffer. */
#pragma bss-name (push, "SCENE")
TI_SCENE_STORAGE(ITEMS, 120);
#pragma bss-name (pop)
/* Every image the scene draws: the sprite cells, the atlas sub-sprites, then
 * solid rectangles for stars, the shield bubble, the ship's core, lasers,
 * railgun slugs and the railgun's aim. The boss images come last: its body
 * strips, then two images on the frame slot of each part. boss_art() points
 * them into the window; until then they are 1x1 rectangles. */
enum { SF_IMG_SUB=SF_ART_COUNT, SF_IMG_FAR=SF_ART_COUNT+SF_SUB_COUNT,
    SF_IMG_DOT=SF_IMG_FAR+SF_FAR_COUNT, SF_IMG_DOT2, SF_IMG_EDGE, SF_IMG_SIDE,
    SF_IMG_LASER, SF_IMG_SLUG, SF_IMG_TRAIL, SF_IMG_DASH, SF_IMG_BOSS,
    SF_IMG_PART=SF_IMG_BOSS+SF_BOSS_BANDS, SF_IMG_COUNT=SF_IMG_PART+2*SF_BOSS_PARTS };
#define CELL(n) {sf_sprite_pixels[n],8,16,8}
#define NONE {0,1,1,0}
static ti_image images[SF_IMG_COUNT]={
    CELL(0),CELL(1),CELL(2),CELL(3),CELL(4),CELL(5),CELL(6),CELL(7),CELL(8),CELL(9),
    CELL(10),CELL(11),CELL(12),CELL(13),CELL(14),CELL(15),SF_SUB_IMAGES,SF_FAR_IMAGES,
    {0,1,1,0},{0,1,2,0},{0,6,1,0},{0,1,12,0},{0,1,12,0},{0,2,40,0},{0,1,16,0},{0,1,4,0},
    NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE,NONE};
#undef NONE
#undef CELL
/* The near arena, $BB00-$BDFF, holds the game state and the variables of
 * game.c, then star columns, the animated palette, two text buffers and the
 * boss's name and colors. */
#pragma bss-name (push, "NEARBSS")
static uint8_t star_x[32];
static uint16_t palette[16];
static char scratch[12], banner[20];
/* The boss's name and its colors for palette slots 13 and 14. */
static char boss_name[12];
static uint16_t boss_colors[2];
/* The values that the HUD shows. hud(1) sets each one before any update. */
static uint8_t hud_hull, hud_bombs, hud_weapon, hud_shield;
static uint8_t hud_sector, hud_progress, hud_multiplier, hud_full;
static uint8_t hud_wing, hud_drive;
static uint32_t hud_score, hud_best;
static uint16_t hud_second;
#pragma bss-name (pop)
static int8_t shake_x, shake_y;
static uint8_t shake, flash;
static const uint16_t *flash_ramp;
static const uint16_t bomb_ramp[6]={0x012,0x124,0x347,0x68A,0x9BE,0xCEF};
static const uint16_t hurt_ramp[6]={0x012,0x201,0x401,0x611,0x822,0xA33};
static const uint16_t pulse_ramp[8]={0xF6B,0xF7C,0xF9D,0xFBE,0xFDF,0xFBE,0xF9D,0xF7C};
static const uint16_t shimmer_ramp[4]={0x2BF,0x6DF,0xAEF,0x6DF};
static const uint8_t enemy_art[5]={SF_ART_SCOUT,SF_ART_DART,SF_ART_GUNSHIP,SF_ART_HEAVY,SF_ART_SWEEPER};
static const uint8_t bullet_art[4]={0,SF_SUB_ORB,SF_SUB_FAN,SF_SUB_NEEDLE};
static const uint8_t star_shift[4]={2,2,1,0};
static const uint8_t star_color[4]={blue,blue,dim,grey};
/* Backdrop aircraft i is 11+53*i across, modulo 228. */
static const uint8_t across[BACKDROP]={11,64,117,170,223,48,101,154,207,32,85,138,
    191,16,69,122,175,0,53,106,159,212,37,90};
#define CAP_WARNING 7
#define CAP_SECTOR 9
#define CAP_STICK 10
#define CAP_PICKUP 11  /* plus the pickup, or what an upgrade became */
#define CAP_WINGMAN_LOST 17
#define CAP_BOSS 18    /* the banner, in red */
static const char *const words[]={"ENDLESS SURVIVAL","READY","POWER UP!","ALL CLEAR!",
    "GREAT SHOT!","EXCELLENT!","GAME OVER","WARNING! HEAVY INBOUND","NEW RECORD!",banner,
    "JOYSTICK ON","JOYSTICK OFF","SHIELD ONLINE","HULL REPAIRED","BOMB STOCKED",
    "WINGMAN READY","OVERDRIVE! POINTS X2","WINGMAN DOWN",banner};

/* The step the game is in, shown when it stops for the first error the
 * library kept: 1 memory, 2 data file, 3 canvas, 4 video, 5 scene, 6 title,
 * 7 play. Each step is checked before the next begins. */
static uint8_t sf_stage;
/* STARFALL.PAK has one entry, DATA: "SF", the overlay count, a zero, then
 * the offset, length and run address of each overlay, in the order below.
 * The overlays stay in that banked resource. window() copies one to the
 * window at the start of the free memory, where it runs.
 * tools/starfall_pack.py writes the file from the linked overlays. */
enum { SF_OVL_TITLE, SF_OVL_PANELS, SF_OVL_PLAY, SF_OVL_BOSS, SF_OVL_ART,
    SF_OVL_COUNT=SF_OVL_ART+SF_BOSSES };
typedef struct packed { uint32_t offset; uint16_t length; uint8_t *run; } packed;
static ti_handle pack;
static packed overlays[SF_OVL_COUNT];
/* The overlay in the window, and the boss whose art follows the boss overlay. */
static uint8_t window_holds=255, art;
static void window(uint8_t id)
{
    if(window_holds==id||ti_error_first())return;
    ti_error_keep(ti_res_read(pack,overlays[id].offset,overlays[id].run,overlays[id].length));
    window_holds=id;art=0;
}
static void boss_art(void);
/* The overlay of play: the boss overlay and its boss's art, or the waves. */
static void play_window(void)
{
    if(!G.boss){window(SF_OVL_PLAY);return;}
    window(SF_OVL_BOSS);
    if(art!=G.boss&&!ti_error_first()){boss_art();art=G.boss;}
}
/* The loader runs from the window before the first overlay replaces it. */
#pragma code-name (push, "LOADER")
#pragma rodata-name (push, "LOADER")
static void load(void)
{
    static uint8_t head[4];
    uint32_t size;
    if(ti_error_keep(ti_pack_load("STARFALL.PAK","DATA",&pack,&size)))return;
    ti_error_keep(ti_res_read(pack,0,head,4));
    if(head[0]!='S'||head[1]!='F'||head[2]!=SF_OVL_COUNT||size<4+sizeof(overlays))
        ti_error_keep(TI_E_FORMAT);
    else ti_error_keep(ti_res_read(pack,4,overlays,sizeof(overlays)));
}
#pragma rodata-name (pop)
#pragma code-name (pop)
/* A real clock gap invalidates the audio clock, which the main loop checks.
 * Other update errors, such as speech that never reports finishing, leave
 * it valid: the game then plays on without speech instead of pausing. */
static void service(void)
{
    if(audio_ready)ti_audio_update();
}
/* The playfield below the caption bar; audio is serviced while it draws. */
static const ti_scene_config scene={0,16,240,184,images,SF_IMG_COUNT,0,service};
static void rect(int16_t x,int16_t y,uint16_t w,uint16_t h,uint8_t c)
{ if(!ti_error_first())ti_error_keep(ti_gfx_rect(&canvas,x,y,w,h,c)); }
static void text(int16_t x,int16_t y,const char *s,uint8_t c)
{ if(!ti_error_first())ti_error_keep(ti_gfx_text(&canvas,x,y,s,c)); }
static void present(int16_t x,int16_t y,uint16_t w,uint16_t h)
{
    ti_rect r;r.x=x;r.y=y;r.width=w;r.height=h;
    if(!ti_error_first())ti_error_keep(ti_gfx_present(&canvas,&r,0));
    service();
}
static void number(int16_t x,int16_t y,uint32_t value,uint8_t n,uint8_t color)
{
    char s[11];
    text(x,y,ti_digits(s,value,n),color);
}
static void reset_clock(void)
{
    if(audio_ready)ti_error_keep(ti_audio_resume_clock());
    ti_steps_reset();direction=bomb=0;sf_lag=0;
}
/* A full list drops the sprite; the screen shake moves everything. */
static void add(int16_t x,int16_t y,uint8_t image,uint8_t color)
{ ti_scene_add(x+shake_x,y+shake_y,image,color); }
static void sprite(int16_t x,int16_t y,uint8_t cell) { add(x-8,y-8,cell,0); }
/* An image centred on x,y. */
static void centred(int16_t x,int16_t y,uint8_t image,uint8_t color)
{
    const ti_image *s=&images[image];
    add(x-s->row_bytes,y-(s->rows>>1),image,color);
}
/* An atlas sub-sprite centred on x,y. */
static void sub(int16_t x,int16_t y,uint8_t id) { centred(x,y,SF_IMG_SUB+id,0); }
static void boss_objects(void);
static void scene_objects(void)
{
    /* A rounded shield bubble: two edges, two sides and four corner pixels. */
    static const int8_t bubble_x[8]={-6,-6,-12,10,-10,8,-10,8},bubble_y[8]={-11,10,-6,-6,-9,-9,7,7};
    static const uint8_t bubble_image[8]={SF_IMG_EDGE,SF_IMG_EDGE,SF_IMG_SIDE,SF_IMG_SIDE,
        SF_IMG_DOT2,SF_IMG_DOT2,SF_IMG_DOT2,SF_IMG_DOT2};
    uint8_t i,k,n,b,x,y,glint,life,lane[6];
    uint16_t now=(uint16_t)G.tick;
    ti_scene_frame();
    if(shake){--shake;shake_x=shake&1?2:-2;shake_y=shake&2?1:-1;}
    else shake_x=shake_y=0;
    /* Three star speeds make forward motion visible without scrolling 32K.
     * Near stars twinkle white for a moment every two seconds. Busy scenes
     * draw fewer stars, then fewer backdrop aircraft, so every pickup,
     * aircraft and shot fits the list: less 16 for effects, 3 pickups, 2
     * wingmen, 13 ship parts and 24 for a boss: its strips, its three
     * parts and the railgun's aim. */
    k=sf_enemy_count+sf_bullet_count+(G.boss?24:0);
    n=k<ITEMS-34?ITEMS-34-k:0;
    /* A boss sector has no backdrop: palette slot 13 is the boss's. */
    b=backdrop&&!G.boss?(n<BACKDROP?n:BACKDROP):0;
    n-=b;
    if(n>32)n=32;
    /* Star i is 47*i plus its speed's share of the tick down, modulo 184.
     * The stars of speed k are entries k, k+4 and so on, each 188 lines, 4
     * modulo 184, below the one before: a speed's lane steps on by 4 a
     * star, and a frame divides once a speed instead of once a star. */
    for(k=0;k<4;++k)lane[k]=(uint8_t)((47U*k+(now>>star_shift[k]))%184);
    for(i=0,glint=(uint8_t)now;i<n;++i,glint+=9) {
        k=i&3;
        add(star_x[i],16+lane[k],k==3?SF_IMG_DOT2:SF_IMG_DOT,k==3&&(glint&63)<3?white:star_color[k]);
        if((lane[k]+=4)>=184)lane[k]-=184;
    }
    /* The backdrop: sixteen small far aircraft moving a pixel every four
     * frames, then eight larger ones every two, each on its own frame.
     * Aircraft i is 71*i+((now+i)>>2) down, or >>1 for the larger ones,
     * modulo 200: the far ones with one image are every fourth entry, 85
     * lines apart, and the larger ones every second, 143 apart. Positions
     * follow the tick, so the backdrop keeps no state. */
    for(k=0;k<6;++k){i=k<4?k:12+k;lane[k]=(uint8_t)((71U*i+((now+i)>>(k<4?2:1)))%200);}
    for(i=0;i<b;++i) {
        k=i<16?i&3:4+(i&1);y=k<4?85:143;
        add(across[i],lane[k],k<4?SF_IMG_FAR+k:SF_ART_MID,0);
        lane[k]=lane[k]>=200-y?lane[k]-(200-y):lane[k]+y;
    }
    /* Everything else passes over the boss. */
    if(G.boss)boss_objects();
    for(i=0;i<SF_PICKUPS;++i)if(G.pickup_active[i])
        sprite(G.pickup_x[i],G.pickup_y[i],SF_ART_POWER+G.pickup_kind[i]);
    for(i=0;i<SF_PLAYER_BULLETS;++i)if(G.shot_active[i])
        sub(G.shot_x[i],G.shot_y[i],G.shot_power[i]>1?SF_SUB_LANCE:SF_SUB_BOLT);
    for(i=0;i<SF_ENEMIES;++i)if(G.enemy_active[i])
        sprite(G.enemy_x[i],G.enemy_y[i],enemy_art[G.enemy_kind[i]]);
    for(i=0;i<SF_WINGMEN;++i)if(G.wing_active[i])sub(G.wing_x[i],G.wing_y[i],SF_SUB_WING);
    if(G.hull) {
        x=G.ship_x;y=G.ship_y;
        sub(x,y+10,now&2?SF_SUB_FLAME1:SF_SUB_FLAME0);
        /* Overdrive doubles the exhaust and lights the core. */
        if(G.overdrive)sub(x,y+15,now&2?SF_SUB_FLAME0:SF_SUB_FLAME1);
        if(!G.invulnerable||(now&2))sprite(x,y,SF_ART_PLAYER);
        if(G.shield)for(k=0;k<8;++k)add(x+bubble_x[k],y+bubble_y[k],bubble_image[k],shimmer);
        /* The bright central two pixels show the player's small hitbox. */
        add(x,y,SF_IMG_DOT2,G.overdrive?pulse:white);
    }
    for(i=0;i<SF_ENEMY_BULLETS;++i)if((k=G.bullet_active[i])) {
        x=G.bullet_x[i];y=G.bullet_y[i];
        if(k<SF_SHOT_LASER)sub(x,y,bullet_art[k]);
        else if(k==SF_SHOT_LASER)centred(x,y,SF_IMG_LASER,red);
        else{centred(x,y-28,SF_IMG_TRAIL,cyan);centred(x,y,SF_IMG_SLUG,white);}
    }
    for(i=0;i<SF_FX;++i)if((life=G.fx_life[i])) {
        x=G.fx_x[i];y=G.fx_y[i];
        if(G.fx_kind[i]==SF_FX_SPARK)sub(x,y,life>3?SF_SUB_SPARK0:SF_SUB_SPARK1);
        else if(G.fx_kind[i]==SF_FX_BIG) {
            /* A heavy breaks up in three staggered bursts. */
            sprite(x,y,SF_ART_EXPLOSION0+(uint8_t)((18-life)/6));
            if(life<15) {
                k=SF_ART_EXPLOSION0+(uint8_t)((15-life)/5);
                sprite(x-9,y-6,k);sprite(x+9,y+5,k);
            }
        } else sprite(x,y,SF_ART_EXPLOSION0+(uint8_t)((12-life)/4));
    }
}
/* Background flashes for bombs and hits; pulsing bullets and shields. This,
 * telemetry() and render() are in low memory, which has room. */
#pragma code-name (push, "LOWCODE")
static void animate(void)
{
    uint8_t phase=(uint8_t)(G.tick>>1);
    palette[0]=flash?flash_ramp[flash--]:sf_palette[0];
    palette[pulse]=pulse_ramp[phase&7];
    if(G.boss){palette[blue]=boss_colors[0];palette[shimmer]=boss_colors[1];}
    else{palette[blue]=sf_palette[blue];palette[shimmer]=shimmer_ramp[phase&3];}
    ti_error_keep(ti_gfx_palette(palette));
}
static void telemetry(void)
{
    static const uint8_t kind_bit[5]={1,2,4,8,16};
    uint8_t i;
    sf_tick=G.tick;sf_score=G.score;sf_hull=G.hull;sf_sector=G.sector;sf_boss_number=G.boss;
    sf_weapon=G.weapon;sf_player_x=G.ship_x;sf_player_y=G.ship_y;
    sf_enemy_count=sf_bullet_count=0;
    for(i=0;i<SF_ENEMIES;++i)if(G.enemy_active[i]) {
        ++sf_enemy_count;sf_enemy_types_seen|=kind_bit[G.enemy_kind[i]];
    }
    for(i=0;i<SF_ENEMY_BULLETS;++i)if(G.bullet_active[i])++sf_bullet_count;
    for(i=0;i<SF_PLAYER_BULLETS;++i)if(G.shot_active[i])++sf_bullet_count;
    if(sf_enemy_count>sf_enemy_peak)sf_enemy_peak=sf_enemy_count;
    if(sf_bullet_count>sf_bullet_peak)sf_bullet_peak=sf_bullet_count;
}
static void render(void)
{
    scene_objects();animate();ti_error_keep(ti_scene_draw());++sf_frames;
}
#pragma code-name (pop)
static void lines(const line *l,uint8_t n)
{
    do{text(l->x,l->y,l->s,l->color);++l;}while(--n);
}
/* Publish changed HUD rows unless the whole panel follows. */
static void hud_present(uint8_t y,uint8_t h)
{ if(!hud_full)present(248,y,72,h); }
/* Replace one HUD value. Each value keeps its length, so the opaque cells
 * cover the old one; only those cells are published. */
static void field(int16_t x,uint8_t y,const char *s,uint8_t color)
{
    if(!ti_error_first())ti_error_keep(ti_gfx_text_opaque(&canvas,x,y,s,color,0));
    if(!hud_full)present(x,y,(uint16_t)strlen(s)*6,8);
}
/* A sixty-pixel gauge. */
static void bar(uint8_t y,uint8_t n,uint8_t color)
{ rect(250,y,60,2,dim);if(n)rect(250,y,n,2,color);hud_present(y,2); }
/* A row of n marks, each step pixels apart. */
static void pips(uint8_t y,uint8_t n,uint8_t step,uint8_t color)
{
    uint8_t i;
    rect(250,y,68,8,0);
    for(i=0;i<n;++i)rect(250+i*step,y+1,step-4,5,color);
}
static void hud(uint8_t force)
{
    static const line labels[]={{250,4,white,"STARFALL"},{250,16,dim,"SECTOR"},
        {250,38,dim,"TIME"},{250,54,dim,"SCORE"},{250,78,dim,"BEST"},
        {250,102,dim,"CHAIN"},{250,118,dim,"HULL"},{250,142,dim,"BOMBS"},
        {250,166,dim,"WEAPON"},{250,190,dim,"P PAUSE"}};
    static const char weapons[4][6]={"SINGLE","TWIN  ","TRIPLE","WIDE  "};
    /* A sector's progress across sixty pixels, or 64 plus the boss's remaining strength. */
    uint8_t progress=G.boss?(uint8_t)(64+(G.boss_full?(uint16_t)(G.part_hp[0]+G.part_hp[1]+G.part_hp[2])*60/G.boss_full:0)):
        (uint8_t)(G.sector_clock/(SF_SECTOR_TICKS/60)),multiplier=sf_multiplier();
    uint8_t sector=G.boss?G.sector|128:G.sector;
    /* Wingmen in the low bits, 4 in overdrive. */
    uint8_t wing=G.wing_active[0]+G.wing_active[1]+(G.overdrive?4:0),drive=G.overdrive>>2;
    uint16_t second=G.seconds;
    uint32_t best=G.score>best_score?G.score:best_score;
    hud_full=force;
    if(force){rect(240,0,80,200,0);rect(240,0,2,200,blue);lines(labels,sizeof(labels)/sizeof(labels[0]));}
    /* A boss sector shows its number in red. */
    if(force||hud_sector!=sector)field(304,16,ti_digits(scratch,G.sector+1,2),(hud_sector=sector)&128?red:cyan);
    if(force||hud_progress!=progress)bar(26,(hud_progress=progress)&63,progress&64?red:cyan);
    if(force||hud_second!=second) {
        ti_digits(scratch,(hud_second=second)/60,2);scratch[2]=':';ti_digits(scratch+3,second%60,2);
        field(286,38,scratch,white);
    }
    if(force||hud_score!=G.score)field(250,64,ti_digits(scratch,hud_score=G.score,7),amber);
    if(force||hud_best!=best)field(250,88,ti_digits(scratch,hud_best=best,7),white);
    if(force||hud_multiplier!=multiplier) {
        scratch[0]='X';ti_digits(scratch+1,hud_multiplier=multiplier,1);
        field(304,102,scratch,multiplier>1?pulse:dim);
    }
    if(force||hud_hull!=G.hull||hud_shield!=G.shield) {
        pips(128,hud_hull=G.hull,12,red);
        if((hud_shield=G.shield))text(286,128,"+",shimmer);
        hud_present(127,10);
    }
    if(force||hud_bombs!=G.bombs){pips(152,hud_bombs=G.bombs,10,amber);hud_present(151,10);}
    /* The weapon name pulses in overdrive, whose gauge counts down beneath.
     * A suffix counts wingmen: TRIPLE+2. */
    if(force||hud_weapon!=G.weapon||hud_wing!=wing) {
        memcpy(scratch,weapons[hud_weapon=G.weapon],6);
        scratch[6]=scratch[7]=' ';scratch[8]=0;
        if((hud_wing=wing)&3){scratch[6]='+';scratch[7]=(char)('0'+(wing&3));}
        field(250,176,scratch,wing&4?pulse:cyan);
    }
    if(force||hud_drive!=drive)bar(186,hud_drive=drive,pulse);
    if(force)present(240,0,80,200);
}
static void caption(uint8_t id)
{
    sf_caption=id;caption_until=G.tick+75;
    rect(4,2,236,11,0);text(8,4,words[id],id==CAP_WARNING||id==CAP_BOSS?red:(id?amber:dim));present(4,2,236,11);
}
static void frame(void)
{
    ti_error_keep(ti_gfx_clear(&canvas,0));
    rect(0,14,240,1,blue);hud(1);present(0,0,320,200);caption(sf_caption);
    /* The canvas now covers the playfield: nothing of the last list shows. */
    ti_scene_forget();
}
/* The title screen and the launch are an overlay; see window(). */
static void restart(void);
#pragma code-name (push, "TITLE")
#pragma rodata-name (push, "TITLE")
static void large(int16_t x,int16_t y,uint8_t color)
{
    static const uint8_t logo[8][7]={
        {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{14,17,17,31,17,17,17},
        {30,17,17,30,20,18,17},{31,16,16,30,16,16,16},{14,17,17,31,17,17,17},
        {16,16,16,16,16,16,31},{16,16,16,16,16,16,31}};
    uint8_t row,col,bits,i;
    for(i=0;i<8;++i){for(row=0;row<7;++row){bits=logo[i][row];
        for(col=0;col<5;++col)if(bits&(16>>col))rect(x+col*3,y+row*3,3,3,color);
    }x+=18;service();}
}
/* The title's backdrop choice. */
static void option(void)
{
    if(!ti_error_first())ti_error_keep(ti_gfx_text_opaque(&canvas,140,158,backdrop?"ON ":"OFF",cyan,0));
    present(140,158,18,8);
}
static void title(void)
{
    static const line title_lines[]={{48,28,cyan,"A LIBTINI ARCADE EXAMPLE"},
        {72,70,amber,"SURVIVE THE RIFT"},{34,108,dim,"25"},{74,108,dim,"40"},
        {111,108,dim,"100"},{154,108,dim,"60"},{191,108,dim,"250"},
        {24,122,white,"WASD / ARROWS"},{140,122,cyan,"HOLD TO MOVE"},
        {24,131,white,"SPACE / B / FIRE"},{140,131,cyan,"BOMB"},
        {24,140,white,"JOYSTICK"},{140,140,cyan,"AUTO / J OFF"},
        {24,149,white,"P / ESC"},{140,149,cyan,"PAUSE"},{24,158,white,"B BACKGROUND"},
        {24,171,white,"M MOCKINGBOARD   P PHASOR"},
        {24,180,amber,"ENTER OR FIRE TO LAUNCH"},{24,189,dim,"ART: KENNEY CC0  MUSIC: DORANARASI"}};
    static const uint8_t roster[5]={SF_ART_SCOUT,SF_ART_DART,SF_ART_GUNSHIP,SF_ART_SWEEPER,SF_ART_HEAVY};
    uint8_t i;
    sf_new(0x5A17);frame();
    rect(4,20,232,176,0);
    for(i=0;i<22;++i)rect(8+(i*71U)%224,23+(i*43U)%60,2,1,i&1?blue:dim);
    large(50,44,blue);large(48,42,white);
    lines(title_lines,sizeof(title_lines)/sizeof(title_lines[0]));
    option();
    present(0,0,240,200);
    /* The enemy roster blocks hold no text, so composed blocks can replace them. */
    ti_scene_frame();
    for(i=0;i<5;++i)sprite(40+i*40,96,roster[i]);
    ti_error_keep(ti_scene_draw());
}
/* The cheat: HELLO, then a sector number of one or two digits. Each run of
 * the session then starts at that sector; see sf_begin. */
static void cheat(uint8_t k)
{
    static const char word[5]={'H','E','L','L','O'};
    if(hello==5) {
        if(k>='0'&&k<='9') {
            warp=(uint8_t)(warp%10*10+k-'0');
            memcpy(scratch,"SECTOR ",7);ti_digits(scratch+7,warp,2);
            if(!ti_error_first())ti_error_keep(ti_gfx_text_opaque(&canvas,176,180,scratch,pulse,0));
            present(176,180,54,8);
            return;
        }
        hello=0;
    }
    hello=k==word[hello]?hello+1:k=='H';
}
/* Nonzero when no audio card answers; main then shows the panel. */
static ti_error start(uint8_t personality)
{
    ti_audio_config config;ti_error e;
    /* Every voice: 12 on a Phasor, 6 on a Mockingboard or a Phasor that
     * the driver found answering as one. */
    config.personality=personality;config.speech=1;config.ducking=1;config.via_hz=0;config.voices=0;
    e=ti_audio_init(&config);
    if(e)return e;
    music_voices=ti_audio_voices()==12?255:3;
    audio_ready=1;ti_speech_bind(&sf_speech);
    ti_steps_begin(ti_audio_ticks,TI_AUDIO_TICKS_PER_SECOND,30,30);restart();
    return TI_OK;
}
#pragma rodata-name (pop)
#pragma code-name (pop)
/* The song's first two tracks are its lead and bass, which a Mockingboard plays alone. */
static void begin_music(void)
{ ti_audio_reserve(music_voices);ti_song_start(sf_song,SF_SONG_SIZE,1); }
/* Queue a phrase and show its caption; 0 when the driver refuses it. */
static uint8_t say(uint8_t id,uint8_t priority)
{
    if(ti_speech_say(id,priority)!=TI_OK)return 0;
    caption(id+1);return 1;
}
static void restart(void)
{
    sf_new(0x5A17);sf_enemy_types_seen=sf_enemy_peak=sf_bullet_peak=0;
    sf_caption=0;flash=shake=0;warp_due=warp;ti_stick_recalibrate();
    sf_mode=1;telemetry();frame();render();reset_clock();begin_music();
    ti_speech_stop();say(SAY_READY,TI_SPEECH_INTERRUPT|2);
}
static void resume(void)
{ sf_mode=1;ti_stick_recalibrate();frame();render();reset_clock();begin_music(); }
static void sector_banner(void)
{
    char *s=banner;
    uint16_t bonus=(uint16_t)G.sector*100;
    memcpy(s,"SECTOR ",7);ti_digits(s+7,G.sector+1,2);
    memcpy(s+9," +",2);ti_digits(s+11,bonus,bonus<1000?3:4);
    caption(CAP_SECTOR);
}
static void effects(uint16_t events)
{
    /* Tone period, effect voice, volume, duration and priority for each
     * event bit from SF_EVENT_FIRE to SF_EVENT_WINGMAN_LOST. Effect voices
     * count from the first one the music leaves. Shots and the final hit
     * play nothing here. Volumes were doubled after board listening: each is
     * the AY gain-table level nearest twice the earlier amplitude, capped at
     * 15. */
    #pragma rodata-name (push, "RODATA")
    static const uint16_t period[12]={150,0,950,1700,284,2000,0,190,90,1900,600,700};
    static const uint8_t voice[12]={0,0,1,2,2,3,0,2,0,1,3,1};
    static const uint8_t volume[12]={7,0,12,15,13,15,0,14,9,15,15,13};
    static const uint8_t duration[12]={3,0,10,20,16,30,0,30,2,36,45,14};
    static const uint8_t priority[12]={1,0,2,4,3,5,0,3,1,4,4,3};
    #pragma rodata-name (pop)
    uint8_t i;
    uint16_t bits=events&(SF_EVENT_TOP_CHAIN-1);
    for(i=0;bits;++i,bits>>=1)if((bits&1)&&period[i])
        ti_audio_effect(ti_audio_effect_voice(voice[i]),period[i],volume[i],duration[i],priority[i]);
    if(events&SF_EVENT_BIG)shake=8;
    if(events&SF_EVENT_HURT){shake=6;flash_ramp=hurt_ramp;flash=5;}
    if(events&SF_EVENT_WINGMAN_LOST)caption(CAP_WINGMAN_LOST);
    /* A weapon step says POWER UP unless another phrase is playing. A repair
     * says EXCELLENT, or shows its own caption when speech is refused. */
    if(events&SF_EVENT_PICKUP) {
        if(G.last_pickup==SF_GOT_OVERDRIVE){flash_ramp=bomb_ramp;flash=5;}
        if(G.last_pickup==SF_UPGRADE){if(!ti_speech_busy())say(SAY_POWER,1);}
        else if(G.last_pickup!=SF_REPAIR||!say(SAY_EXCELLENT,2))caption(CAP_PICKUP+G.last_pickup);
    }
    /* A boss's arrival and its end show its name, not the phrase's caption,
     * and replace the sector's banner. */
    if(events&SF_EVENT_BOSS_DOWN) {
        strcpy(banner,boss_name);strcat(banner," DESTROYED");
        caption(CAP_SECTOR);ti_speech_say(SAY_EXCELLENT,TI_SPEECH_INTERRUPT|2);
    } else if(events&SF_EVENT_BOSS) {
        memcpy(banner,"WARNING! ",9);strcpy(banner+9,boss_name);
        caption(CAP_BOSS);ti_speech_say(SAY_WARNING,TI_SPEECH_INTERRUPT|2);
    } else if(events&SF_EVENT_SECTOR)sector_banner();
    if(events&SF_EVENT_BOMB){say(SAY_CLEAR,TI_SPEECH_INTERRUPT|2);flash_ramp=bomb_ramp;flash=5;shake=6;}
    if(events&SF_EVENT_WARNING)say(SAY_WARNING,TI_SPEECH_INTERRUPT|2);
    /* Queued behind any line above, so a bomb that reaches x8 says both. */
    if(events&SF_EVENT_TOP_CHAIN)say(SAY_GREAT,2);
    if(sf_caption&&G.tick>=caption_until)caption(0);
}
/* The pause and game-over panels are an overlay; see window(). */
#pragma code-name (push, "PANELS")
#pragma rodata-name (push, "PANELS")
static void panel(int16_t y,uint16_t h)
{ rect(12,y,216,h,0);rect(12,y,216,1,cyan);rect(12,y+h-1,216,1,blue); }
static void overlay(const char *heading,const char *sub)
{
    panel(70,66);text(26,80,heading,white);text(26,98,sub,amber);
    text(26,118,"R RETRY    Q QUIT",dim);present(12,70,216,66);stick_shown=0;
}
/* While paused, the panel shows live joystick readings for checking a stick. */
static void show_stick(void)
{
    static uint8_t shown_x,shown_y;
    if(ti_joystick_read(&sf_joystick)==TI_E_BUSY)return;
    if(stick_shown&&sf_joystick.x==shown_x&&sf_joystick.y==shown_y)return;
    stick_shown=1;
    memcpy(scratch,"JOY ",4);ti_digits(scratch+4,shown_x=sf_joystick.x,3);
    scratch[7]=' ';ti_digits(scratch+8,shown_y=sf_joystick.y,3);
    rect(26,108,96,8,0);text(26,108,scratch,cyan);present(26,108,96,8);
}
static void paused(void) { overlay("PAUSED","P / ENTER / FIRE: RESUME"); }
static void clock_gap(void) { overlay("CLOCK GAP / PAUSED","P / ENTER / FIRE: RESUME"); }
static void no_audio(void) { overlay("SLOT 4 AUDIO REQUIRED","CONFIGURE MB OR PHASOR"); }
static void game_over(void)
{
    static const line over_lines[]={{26,62,white,"SHIP LOST"},{26,78,dim,"SCORE"},
        {26,90,dim,"SECTOR"},{110,90,dim,"KILLS"},{26,122,amber,"ENTER / FIRE: TRY AGAIN"},
        {26,134,dim,"Q: QUIT"}};
    uint8_t record=best_score&&G.score>best_score;
    sf_mode=2;over_at=ti_audio_ticks();flash=0;animate();
    ti_music_stop();ti_speech_stop();say(SAY_OVER,TI_SPEECH_INTERRUPT|3);
    if(record)ti_speech_say(SAY_RECORD,2);
    if(G.score>best_score)best_score=G.score;
    panel(54,96);lines(over_lines,sizeof(over_lines)/sizeof(over_lines[0]));
    number(80,78,G.score,7,amber);number(80,90,G.sector+1,2,cyan);number(146,90,G.kills,5,cyan);
    if(record)text(26,102,"NEW BEST!",pulse);
    else{text(26,102,"BEST",dim);number(80,102,best_score,7,white);}
    present(12,54,216,96);hud(0);
}
#pragma rodata-name (pop)
#pragma code-name (pop)
/* The boss's drawing is part of the boss overlay; see play_window(). The
 * boss's record is copied to sf_boss, and its art follows the overlay in the
 * window: the body strips, then one frame slot for each part. A part's
 * frames stay in the pack, and a new frame switches the part to its other
 * image on the same slot, so the scene draws it again. */
#pragma code-name (push, "BOSS")
#pragma rodata-name (push, "BOSS")
#pragma bss-name (push, "BOSSBSS")
extern sf_boss_art sf_boss;
static uint32_t art_at;
static uint8_t shown[SF_BOSS_PARTS],flip[SF_BOSS_PARTS],i,f;
static void boss_art(void)
{
    const packed *o=&overlays[SF_OVL_ART+G.boss-1];
    ti_image *m=&images[SF_IMG_BOSS];
    art_at=o->offset;
    ti_error_keep(ti_res_read(pack,art_at+4,&sf_boss,sizeof(sf_boss)));
    ti_error_keep(ti_res_read(pack,art_at+4+sizeof(sf_boss),o->run,sf_boss.strip_bytes));
    /* The record's images have window offsets; ti_image has the same layout. */
    memcpy(m,sf_boss.images,sizeof(sf_boss.images));
    for(i=0;i<SF_BOSS_BANDS+2*SF_BOSS_PARTS;++i,++m)if(m->stride)m->pixels+=(uint16_t)o->run;
    shown[0]=shown[1]=shown[2]=255;
    boss_colors[0]=sf_boss.colors[0];boss_colors[1]=sf_boss.colors[1];
    memcpy(boss_name,sf_boss.name,sizeof(boss_name));
    /* The new images are checked, and the whole playfield is drawn again. */
    ti_error_keep(ti_scene_begin(&scene));ti_scene_invalidate();
}
static void boss_objects(void)
{
    int16_t x=G.boss_x,top=(int16_t)G.boss_y-sf_boss.height;
    for(i=0;i<sf_boss.bands;++i)add(x+sf_boss.band_x[i],top+sf_boss.band_y[i],SF_IMG_BOSS+i,0);
    /* Eight frames a second: intact, or destroyed. */
    for(i=0;i<SF_BOSS_PARTS;++i) {
        f=(G.part_hp[i]?0:4)|((uint8_t)G.tick>>2&3);
        if(f!=shown[i]&&!ti_error_first()) {
            shown[i]=f;flip[i]^=1;
            ti_error_keep(ti_res_read(pack,art_at+sf_boss.frames_at[i]+f*sf_boss.frame_bytes[i],
                (uint8_t *)images[SF_IMG_PART+2*i].pixels,sf_boss.frame_bytes[i]));
        }
        add(x+sf_boss.part_x[i],top+sf_boss.part_y[i],SF_IMG_PART+2*i+(flip[i]&1),0);
    }
    /* A charging railgun shows its column, blinking. */
    if(G.rail_clock&&(G.tick&2))
        for(i=G.boss_y+4;i<SF_BOTTOM;i+=12)add(G.rail_x,i,SF_IMG_DASH,red);
}
#pragma bss-name (pop)
#pragma rodata-name (pop)
#pragma code-name (pop)
/* The stop screen's text lives in low memory, which has room in both builds. */
#pragma rodata-name (push, "RODATA")
static const char stop_text[]="ERROR    STEP ",key_text[]="PRESS A KEY";
#pragma rodata-name (pop)
/* One 40-column text row, padded with spaces. */
static void show(uint8_t *row,const char *s)
{ for(key=0;key<40;++key)row[key]=(uint8_t)(*s?*s++:' ')|128; }
/* Calibrated joystick directions, which match the SF_IN bits, or 255 when
 * no stick answers. Each launch, retry and resume measures the rest again. */
static uint8_t read_stick(void)
{
    uint8_t bits;
    if(!stick_enabled){sf_stick=0;return 255;}
    bits=ti_stick_read(&sf_joystick);
    if(bits==TI_STICK_ABSENT){sf_stick=2;return 255;}
    sf_stick=1;return bits;
}
/* WASD and the arrow keys. */
static const char move_key[8]={'W',11,'S',10,'A',8,'D',21};
static const uint8_t move_dir[8]={SF_IN_UP,SF_IN_UP,SF_IN_DOWN,SF_IN_DOWN,
    SF_IN_LEFT,SF_IN_LEFT,SF_IN_RIGHT,SF_IN_RIGHT};
/* Keys move the ship while held; a tap shorter than a frame still moves it
 * once. The //e reports only the newest key, so another key held on top
 * keeps the current direction until every key is up. */
static void held_direction(void)
{
    uint8_t i,k=ti_input_held();
    if(k>='a'&&k<='z')k-=32;
    if(k){for(i=0;i<sizeof(move_key);++i)if(k==move_key[i])direction=move_dir[i];}
    else if(!tapped)direction=0;
    tapped=0;
}
int main(void)
{
    uint8_t ticks,input,pressed,i;uint16_t events;
    /* The overlays, the canvas and AUX backup plus the temporary ProDOS/IRQ
     * storage loan need more than two 48,640-byte resource banks. */
    sf_stage=1;if(ti_error_keep(ti_init(1,3)))goto done;
    /* Before graphics, so the file calls need no storage loan. */
    sf_stage=2;load();window(SF_OVL_TITLE);if(ti_error_first())goto done;
    for(i=0;i<32;++i)star_x[i]=(uint8_t)(6+(i*73U)%226);
    memcpy(palette,sf_palette,sizeof(sf_palette));
    sf_stage=3;if(ti_error_keep(ti_gfx_surface_create(&canvas,320,200)))goto done;
    sf_stage=4;if(ti_error_keep(ti_gfx_begin(sf_palette)))goto done;
    sf_stage=5;if(ti_error_keep(ti_scene_begin(&scene)))goto done;
    sf_stage=6;title();telemetry();
    for(;;) {
        if(ti_error_first())goto done;
        sf_stage=7;
        if(sf_mode==1)play_window();
        else window(sf_mode?SF_OVL_PANELS:SF_OVL_TITLE);
        service();ti_input_poll();
        /* Either joystick button acts as fire: launch, bomb, resume, retry. */
        i=ti_input_buttons()&3;pressed=i&&!last_buttons;last_buttons=i;
        while(ti_input_key(&key)==TI_OK) {
            if(key>='a'&&key<='z')key-=32;
            if(key=='Q')goto done;
            if(sf_mode==0) {
                if(key=='B'){backdrop^=1;option();}
                else if(key=='M'||key=='P'||key==13) {
                    if(start(key=='P'?TI_AUDIO_PHASOR:TI_AUDIO_MOCKINGBOARD)){window(SF_OVL_PANELS);no_audio();}
                }
                else cheat(key);
                continue;
            }
            if(key=='R'||(sf_mode==2&&key==13)){restart();continue;}
            if(sf_mode==2)continue;
            if(key=='P'||key==27||(sf_mode==3&&key==13)) {
                if(sf_mode==3){play_window();resume();}
                else{sf_mode=3;flash=0;animate();ti_music_stop();window(SF_OVL_PANELS);paused();}
                continue;
            }
            if(sf_mode!=1)continue;
            if(key=='J'){stick_enabled^=1;direction=0;ti_stick_recalibrate();caption(CAP_STICK+!stick_enabled);}
            for(i=0;i<sizeof(move_key);++i)if(key==move_key[i]){direction=move_dir[i];tapped=1;}
            if(key==' '||key=='B')bomb=SF_IN_BOMB;
        }
        if(pressed) {
            if(sf_mode==0){if(start(TI_AUDIO_MOCKINGBOARD)){window(SF_OVL_PANELS);no_audio();}}
            /* A fire button held through the crash cannot skip this panel. */
            else if(sf_mode==2){if(ti_audio_ticks()-over_at>100)restart();}
            else if(sf_mode==3){play_window();resume();}
            else bomb=SF_IN_BOMB;
        }
        if(sf_mode==3&&stick_enabled)show_stick();
        if(sf_mode!=1)continue;
        if(sf_lag||!ti_audio_clock_valid()) {
            sf_mode=3;ti_music_stop();window(SF_OVL_PANELS);clock_gap();continue;
        }
        /* Thirty updates a second from the audio clock, at most a second's
         * worth per drawn frame; a longer gap pauses. */
        if((ticks=ti_steps_due())==TI_STEPS_GAP){sf_lag=1;continue;}
        if(!ticks)continue;
        events=0;
        held_direction();
        /* A deflected stick overrides the key direction. */
        input=read_stick();
        if(input&&input!=255)direction=0;
        else input=direction;
        /* A cheat start sets up the run before its first step. */
        if(warp_due){play_window();sf_begin(warp_due);warp_due=0;}
        /* Each step starts with its overlay: a boss can arrive or leave. */
        for(i=0;i<ticks&&!G.game_over;++i){play_window();events|=sf_step(input|bomb);bomb=0;++sf_steps;}
        play_window();effects(events);telemetry();render();hud(0);
        if(G.game_over){window(SF_OVL_PANELS);game_over();}
    }
done:
    ti_audio_shutdown();ti_gfx_end();if(canvas.pixels)ti_gfx_surface_free(&canvas);ti_shutdown();
    if(ti_error_first()) {
        /* Show the error on the 40-column text screen until a key is pressed:
         * ProDOS's selector would clear it at once. */
        /* "ERROR ee STEP s": a libtini error code and the step above. */
        memcpy(banner,stop_text,14);ti_digits(banner+6,ti_error_first(),2);
        banner[8]=' ';banner[14]=(char)('0'+sf_stage);banner[15]=0;
        *(volatile uint8_t *)0xC00C=0;*(volatile uint8_t *)0xC00E=0;i=*(volatile uint8_t *)0xC051;
        show((uint8_t *)0x0400,banner);show((uint8_t *)0x0480,key_text);
        i=*(volatile uint8_t *)0xC010;
        while(!(*(volatile uint8_t *)0xC000&128)){}
        i=*(volatile uint8_t *)0xC010;
    }
    return ti_error_first()?1:0;
}
