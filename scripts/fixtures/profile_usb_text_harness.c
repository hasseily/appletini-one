/* USB transport/registers use existing host stubs. Menu/editor/routing code is
 * inserted directly from production; only filesystem and unrelated pages stub. */
#include "config_menu_internal.h"
#include "profile_manager.h"
#include <assert.h>

typedef struct {uint32_t frame,key_count,input_seq;ui_key_t last_key;} ui_state_t;
enum {SCREENSHOT_SERVICE_KIND_A2,SCREENSHOT_SERVICE_KIND_1080P};
static uint8_t g_usb_menu_owned,g_onee_standby_available;
static unsigned saved,created,captures,normal_keys,tabs,refreshes,screenshots;
static char saved_name[128],saved_path[128];
static usb_hid_menu_source_t captured;
static ui_key_t last_normal;
static uint32_t mock_onee_status;

/* DECLARATIONS */

static int http_download_active(void){return 0;}
uint8_t config_menu_usb0_sd_remote_active(const config_menu_t *m){return m->usb0_sd_remote_active;}
uint8_t config_menu_ethernet_ftp_sd_remote_active(const config_menu_t *m){return m->ethernet_ftp_sd_remote_active;}
uint8_t config_menu_capture_usb_binding(config_menu_t *m,usb_hid_menu_source_t s){++captures;captured=s;m->usb_binding_capture=CONFIG_MENU_USB_BIND_CAPTURE_NONE;return 1;}
static void ui_request_boot_menu_close(config_menu_t *m){m->active=0;}
static void ui_handle_input(ui_state_t *s,ui_input_t in){(void)s;(void)in;}
static void ui_set_boot_menu_visible(ui_state_t *s,config_menu_t *m,uint8_t on){(void)s;m->active=on;}
static void ui_toggle_usb_gamepad_menu(ui_state_t *s,config_menu_t *m){(void)s;m->active=!m->active;}
static void ui_sync_onee_standby(config_menu_t *m){(void)m;}
static uint8_t ui_onee_selected(void){return (mock_onee_status&CARD_CTRL_ONEE_STATUS_SELECTED_BIT)!=0;}
static void ui_sync_onee_menu_pause(config_menu_t *m){(void)m;}
static void ui_save_screenshot(unsigned kind){(void)kind;++screenshots;}
static void screenshot_service_show_notice(const char *text){(void)text;}
static void vtw_service_speed_toggle(void){}
static void vtw_service_speed_step(int delta){(void)delta;}
static void vtw_service_slug_toggle(void){}
static const char *vtw_service_last_action_text(void){return "speed";}
uint8_t config_menu_handle_usb_fixed_tab(config_menu_t *m,int direction){(void)m;(void)direction;++tabs;return 1;}
void config_menu_focus_onee_setup(config_menu_t *m){(void)m;}
usb_hid_menu_source_t config_menu_usb_ok_binding_source(const config_menu_t *m){return m->usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_OK];}
usb_hid_menu_source_t config_menu_usb_open_close_binding_source(const config_menu_t *m){return config_menu_usb_ok_binding_source(m);}
usb_hid_menu_source_t config_menu_usb_screenshot_a2_binding_source(const config_menu_t *m){return m->usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2];}
usb_hid_menu_source_t config_menu_usb_screenshot_1080p_binding_source(const config_menu_t *m){return m->usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_1080P];}
usb_hid_menu_source_t config_menu_usb_vtw_binding_source(const config_menu_t *m,uint32_t action){return m->usb_menu_bindings[action];}
static void profile_refresh_carousel(config_menu_t *m){(void)m;++refreshes;}
static void profile_move(config_menu_t *m,int delta){(void)m;(void)delta;}
static void profile_select(config_menu_t *m){(void)m;}
uint8_t profile_manager_is_root(const char *p){return strcmp(p,"0:/profiles")==0;}
uint8_t profile_manager_parent_path(const char *p,char *out,size_t cap){(void)p;snprintf(out,cap,"0:/profiles");return 1;}
FRESULT profile_manager_create_profile(const char *parent,const char *name,char *out,size_t cap){++created;snprintf(saved_name,sizeof(saved_name),"%s",name);snprintf(out,cap,"%s/%s",parent,name);return FR_OK;}
FRESULT profile_manager_rename_profile(const char *p,const char *n,char *out,size_t cap){(void)p;return profile_manager_create_profile("0:/profiles",n,out,cap);}
uint8_t config_menu_save_profile_settings(config_menu_t *m,const char *p){(void)m;++saved;snprintf(saved_path,sizeof(saved_path),"%s",p);return 1;}
uint8_t config_menu_handle_input(config_menu_t *m,ui_input_t in){if(config_menu_profiles_handle_input(m,in))return 1;if(m->item_focus==2&&in.key==UI_KEY_ENTER){config_menu_profiles_save_as(m);return 1;}++normal_keys;last_normal=in.key;return 1;}

/* PRODUCTION_MENU */

#define CHECK(c,msg) do{if(!(c)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,msg);return 1;}}while(0)
static config_menu_t menu;
static ui_state_t ui;
static unsigned drain(void){usb_hid_menu_event_t e;unsigned n=0;while(usb_hid_service_pop_menu_event(&e)){++n;ui_handle_usb_menu_event(&ui,&menu,&e);ui_sync_usb_menu_capture(&menu);}return n;}
static void report(uint8_t usage,uint8_t modifier){struct usb_hid_kbd_report r={0};r.modifier=modifier;r.key[0]=usage;hid_process_boot_keyboard_report(&g_hid_slots[0],(const uint8_t*)&r,sizeof(r));}
static unsigned key(uint8_t usage,uint8_t modifier){report(usage,modifier);unsigned n=drain();report(0,0);return n+drain();}
static void setup(uint8_t fixed,uint8_t standby,uint8_t owned){
 memset(&menu,0,sizeof(menu));memset(&ui,0,sizeof(ui));memset(g_hid_slots,0,sizeof(g_hid_slots));
 g_menu_event_count=g_menu_event_rd=g_menu_event_wr=0;g_menu_capture=g_menu_text_capture=g_binding_capture=0;
 g_onee_fixed_mode=fixed;g_onee_standby_mode=standby;g_onee_input_blocked=1;g_usb_menu_owned=owned;
 menu.active=1;menu.usb_binding_capture=CONFIG_MENU_USB_BIND_CAPTURE_NONE;
 mock_onee_status=fixed?(CARD_CTRL_ONEE_STATUS_SELECTED_BIT|CARD_CTRL_ONEE_STATUS_REQUEST_BIT|CARD_CTRL_ONEE_STATUS_EFFECTIVE_BIT):0;
 menu.onee_mode_status=mock_onee_status;
 menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_OK]=USB_HID_MENU_ACTION_SELECT;
 g_hid_slots[0].active=1;g_hid_slots[0].boot_keyboard=1;
 saved=created=captures=normal_keys=tabs=refreshes=screenshots=0;saved_name[0]=saved_path[0]=0;
 ui_sync_usb_menu_capture(&menu);
}
static void open_save(void){config_menu_profiles_save_as(&menu);ui_sync_usb_menu_capture(&menu);}
static unsigned pad(uint32_t buttons){g_hid_slots[1].gamepad_buttons=buttons;gamepad_menu_process_buttons(&g_hid_slots[1]);return drain();}

int main(void){
 for(unsigned mode=0;mode<2;mode++){
  setup(!mode,mode,1);open_save();
  CHECK(menu.profile_name_editor_active&&g_menu_text_capture&&menu.profile_name_editor_virtual==mode,"running native editor and standby virtual editor both capture USB text");
  menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_OK]=usb_hid_menu_source_from_keyboard_usage(HID_KBD_USAGE_A);
  menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_SCREENSHOT_A2]=usb_hid_menu_source_from_keyboard_usage((HID_KBD_USAGE_A+1U));
  ui_sync_usb_menu_capture(&menu);
  CHECK(key(HID_KBD_USAGE_A,2)==1&&key((HID_KBD_USAGE_A+1U),0)==1,"real boot keyboard reports yield one printable event each");
  CHECK(strcmp(menu.profile_name_editor_text,"Ab")==0&&!saved&&!screenshots,"text beats conflicting OK/screenshot bindings");
  report((HID_KBD_USAGE_A+2U),0);CHECK(drain()==1,"new C edge");report((HID_KBD_USAGE_A+2U),0);CHECK(drain()==0,"held C does not repeat");report(0,0);drain();
  key(HID_KBD_USAGE_DELETE,0);CHECK(strcmp(menu.profile_name_editor_text,"Ab")==0,"Backspace deletes a name character");
  key(HID_KBD_USAGE_ENTER,0);CHECK(saved==1&&created==1&&!strcmp(saved_name,"Ab")&&!strcmp(saved_path,"0:/profiles/Ab")&&!menu.profile_name_editor_active,"USB Enter commits the typed profile via production editor");
  CHECK(!g_menu_text_capture&&mock_key_count==0,"editor exit releases text focus; blocked guest-facing report stays empty");
  open_save();key(HID_KBD_USAGE_A,0);key(HID_KBD_USAGE_ESCAPE,0);CHECK(!menu.profile_name_editor_active&&saved==1&&menu.active,"USB Esc cancels whole modal without closing menu or saving");
 }
 puts("PASS ONEe running/standby USB Save As, conflicting shortcuts, held keys, Backspace/Enter/Esc and blocked guest-facing reports");
 setup(1,0,1);open_save();key(HID_KBD_USAGE_DELETE,0);CHECK(menu.profile_name_editor_active&&!menu.profile_name_editor_text[0],"Backspace on empty text does not cancel");
 key(HID_KBD_USAGE_CAPSLOCK,0);key(HID_KBD_USAGE_A,0);key((HID_KBD_USAGE_A+1U),2);key(HID_KBD_USAGE_CAPSLOCK,0);key(HID_KBD_USAGE_KPD1,0);
 CHECK(!strcmp(menu.profile_name_editor_text,"Ab1"),"caps, Shift inversion and keypad typing");
 menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_OK]=usb_hid_menu_source_from_gamepad_button(USB_GAMEPAD_BUTTON_Y);ui_sync_usb_menu_capture(&menu);
 CHECK(pad(UINT32_C(1)<<USB_GAMEPAD_BUTTON_Y)==1&&menu.profile_name_editor_virtual&&!saved,"Y shows keyboard once despite saved Y=OK");
 CHECK(pad(UINT32_C(1)<<USB_GAMEPAD_BUTTON_Y)==0&&menu.profile_name_editor_virtual,"held Y cannot toggle repeatedly");pad(0);
 CHECK(pad(UINT32_C(1)<<USB_GAMEPAD_BUTTON_Y)==1&&!menu.profile_name_editor_virtual&&!saved,"second Y edge hides keyboard once");pad(0);
 key(HID_KBD_USAGE_ESCAPE,0);
 open_save();menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_OK]=USB_HID_MENU_ACTION_SELECT;ui_sync_usb_menu_capture(&menu);
 pad(UINT32_C(1)<<USB_GAMEPAD_BUTTON_Y);pad(0);
 CHECK(pad(UINT32_C(1)<<USB_GAMEPAD_BUTTON_A)==1&&!strcmp(menu.profile_name_editor_text,"A"),"gamepad A selects virtual A key");pad(0);
 menu.profile_name_editor_vk_index=CONFIG_PROFILE_VK_OK;pad(UINT32_C(1)<<USB_GAMEPAD_BUTTON_A);pad(0);
 CHECK(saved==1&&!strcmp(saved_name,"A"),"gamepad virtual OK commits");
 puts("PASS Caps/Shift/keypad and gamepad Y keyboard visibility, hold edge, binding conflict and virtual-grid commit");
 setup(0,0,0);open_save();ui_handle_input_with_config(&ui,&menu,(ui_input_t){UI_KEY_NONE,1,'P'});ui_handle_input_with_config(&ui,&menu,(ui_input_t){UI_KEY_NONE,1,'h'});ui_handle_input_with_config(&ui,&menu,(ui_input_t){UI_KEY_BACK,1,0});ui_handle_input_with_config(&ui,&menu,(ui_input_t){UI_KEY_ENTER,1,0});
 CHECK(saved==1&&!strcmp(saved_name,"P"),"physical Apple direct input preserves typing, back and enter semantics");
 setup(0,0,0);menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_RIGHT]=usb_hid_menu_source_from_keyboard_usage(HID_KBD_USAGE_A);ui_sync_usb_menu_capture(&menu);key(HID_KBD_USAGE_A,0);
 CHECK(normal_keys==1&&last_normal==UI_KEY_RIGHT,"custom keyboard mapping outside editor remains unchanged");
 open_save();menu.usb_binding_capture=CONFIG_MENU_USB_BIND_ACTION_OK;ui_sync_usb_menu_capture(&menu);key((HID_KBD_USAGE_A+2U),0);
 CHECK(captures==1&&captured==usb_hid_menu_source_from_keyboard_usage((HID_KBD_USAGE_A+2U))&&!menu.profile_name_editor_text[0],"binding capture wins over name text");
 puts("PASS physical Apple keyboard, unchanged custom mapping outside text and binding capture precedence");
 setup(1,0,1);menu.item_focus=2;
 ui_handle_input_with_config(&ui,&menu,(ui_input_t){UI_KEY_ENTER,1,0});
 CHECK(menu.profile_name_editor_active&&g_menu_text_capture,"real UI wrapper captures text immediately after Save As, before batch/frame sync");
 key(HID_KBD_USAGE_A,0);CHECK(!strcmp(menu.profile_name_editor_text,"a"),"a USB report between modal open and frame sync reaches editor");
 report(HID_KBD_USAGE_ENTER,0);report(HID_KBD_USAGE_A+25U,0);CHECK(drain()==2,"Enter and trailing text were already queued");
 CHECK(saved==1&&!strcmp(saved_name,"a")&&!menu.profile_name_editor_active&&!g_menu_text_capture&&!normal_keys,"trailing queued text after commit cannot become a shortcut");
 puts("PASS immediate text-focus publication before batch checkpoint and stale queued-text rejection after commit");
 setup(0,0,1);open_save();menu.usb_menu_bindings[CONFIG_MENU_USB_BIND_ACTION_OK]=usb_hid_menu_source_from_gamepad_button(USB_GAMEPAD_BUTTON_Y);ui_sync_usb_menu_capture(&menu);g_hid_slots[1].generic_axes=1;
 unsigned generic_events=pad(UINT32_C(1)<<USB_GAMEPAD_BUTTON_Y);generic_events+=pad(0);
 CHECK(generic_events==1&&menu.profile_name_editor_virtual&&!strcmp(menu.profile_name_editor_text,"A")&&!saved,"generic axis button3 keeps explicit OK binding without implicit Y toggle");
 puts("PASS generic controller button3 explicit binding stays distinct from normalized Y/Triangle keyboard toggle");
 return 0;
}
