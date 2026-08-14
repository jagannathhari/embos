#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <inttypes.h>
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "cimgui_impl.h"
#define GLAD_GL_IMPLEMENTATION
#include "gl.h"
#include <GLFW/glfw3.h>
#ifdef _MSC_VER
#include <windows.h>
#endif

#ifdef IMGUI_HAS_IMSTR
#define igBegin igBegin_Str
#define igSliderFloat igSliderFloat_Str
#define igCheckbox igCheckbox_Str
#define igColorEdit3 igColorEdit3_Str
#define igButton igButton_Str
#endif

#define igGetIO igGetIO_Nil
#define FILE_EXT ".jnt"
#define JH_APP_NAME "Emboss"
#define JH_CONFIG_FILE "emboss.db"
#define ARR_LEN(arr) (sizeof(arr)/sizeof(arr[0]))
#define EDIT_FONT_SIZE_MAX 32
#define UI_SCALE_MAX 2
#define vpush(arr, x) do { \
    if ((arr).count == (arr).capacity) { \
        (arr).capacity = (arr).capacity == 0 ? (1<<10) : (arr).capacity << 1; \
        (arr).items = realloc((arr).items, (arr).capacity * sizeof(*(arr).items)); \
    } \
    (arr).items[(arr).count++] = (x); \
} while(0)

#define vreserve(arr, n) do { \
    if ((arr).capacity < (n)) { \
        (arr).capacity = (n); \
        (arr).items = realloc((arr).items, (arr).capacity * sizeof(*(arr).items)); \
    } \
} while(0)

#define vat(arr, i) ((arr).items[i])
#define vpop(arr)   ((arr).items[--(arr).count])
#define vclear(arr) ((arr).count = 0)

#define MIN(a,b) (((a)<(b)) ? (a):(b))
#define MAX(a,b) (((a)>(b)) ? (a):(b))
#define MIN_CONTENT_BUFFER (1<<12) //4kb

typedef struct {
	char** items;
	size_t count;
	size_t capacity;
}StringArr;

typedef struct {
    char buf[64];
    int len;
    int pending;
    double t;
} KeysMotion;


typedef enum{
	SCREEN_WELCOME,
	SCREEN_INIT,
	SCREEN_MAIN,
	SCREEN_EDIT
}CurrenScreen;

#include "./vendor/sqlite3.h"
#include "./back.c"
#include "./vendor/tinyfiledialogs.c"
#include "backend_api.h"
#include "icon_font.h"
#include "icon_moon.h"

typedef enum{
	SCALE_1x,
	SCALE_1_25x,
	SCALE_1_5x,
	SCALE_2x,
}UiScale;

typedef struct{
	const char* key;
	const char* value;
}KeyVal;

typedef struct {
	StringArr recent_files;
	const KeyVal* cfg;
	const char*  last_opened;
	int          window_height;
	int          window_width;
	int          window_x;
	int          window_y;
	int          autosave_interval_seconds;
	bool         auto_save;
	int64_t      file_opened_at;
	int          cfg_count;
	int          edit_font_size;
	bool         should_wrap;
	UiScale      ui_scale;
} AppConfig;

GLFWwindow *window;
ImFont *icon_font;
AppConfig app_cfg;
char config_folder[OS_PATHMAX];
char config_file[OS_PATHMAX];
CurrenScreen curr_screen = SCREEN_WELCOME;
int scale_idx = -1; //ui scale 
static KeysMotion motions = {0};
ImGuiIO *ioptr = NULL;
bool focus_once = true;

static char current_file[1 << 13];
static int edit_mode = 0;
static Page current_page = {.content_len = 0, .id = -1};
static BackendDescriptor bck_des;
static Notebook notebook;

bool jh_radio_smallbutton(const char *label, int *v, int id)
{
	bool is_selected = (*v == id);
	if (is_selected) {
		const ImVec4 *pressed_color =
		    igGetStyleColorVec4(ImGuiCol_ButtonActive);
		igPushStyleColor_Vec4(ImGuiCol_Button, *pressed_color);
	}
	bool clicked = igSmallButton(label);
	if (is_selected) {
		igPopStyleColor(1);
	}
	if (clicked) {
		if (is_selected) {
			*v = -1;
			return false;
		}
		*v = id;
		return true;
	}
	return false;
}

bool ok_cancle_dialog(const char* title , const char*msg)
{
	if (igBeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
	    igText(msg);

	    if (igButton("OK", (ImVec2){120, 0})) {
		igCloseCurrentPopup();
		return true;
	    }

	    igSameLine(0, -1);

	    if (igButton("Cancel", (ImVec2){120, 0})) {
		igCloseCurrentPopup();
		return false;
	    }

	    igEndPopup();
	}
	return false;
}


void motion_keys_update(float timeout)
{
	if (!ioptr)
		return;
	double now = igGetTime();

	if (motions.pending && (now - motions.t) > timeout) {
		memset(&motions, 0, sizeof(motions));
	}

	for (int i = 0; i < ioptr->InputQueueCharacters.Size; i++) {
		motions.buf[motions.len++] = (char)ioptr->InputQueueCharacters.Data[i];
		motions.buf[motions.len] = '\0';
		motions.t = now;
		motions.pending = 1;
	}
}




static void print_app_cfg(const AppConfig *cfg)
{
	if (!cfg) return;

	puts("-----------Recent Files-----------");
	for (int i = 0; i < cfg->recent_files.count; i++)
		puts(cfg->recent_files.items[i] ? cfg->recent_files.items[i] : "(null)");
	puts("----------------------------------");

	printf("Window Width  : %d\n", cfg->window_width);
	printf("Window Height : %d\n", cfg->window_height);
	printf("Window X      : %d\n", cfg->window_x);
	printf("Window Y      : %d\n", cfg->window_y);
	printf("Last opened: %s\n", cfg->last_opened ? cfg->last_opened : "(none)");
	printf("Opened At     : %"PRId64"\n", cfg->file_opened_at);
	printf("Autosave      : %s\n", cfg->auto_save ? "enabled" : "disabled");
	printf("Autosave Int  : %ds\n", cfg->autosave_interval_seconds);
	printf("UI Scale: %d\n", cfg->ui_scale);
}


static int create_tables(sqlite3 *db)
{
	char *err = NULL;
	if (sqlite3_exec(db,
		"CREATE TABLE IF NOT EXISTS config ("
		"  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  key        TEXT UNIQUE NOT NULL,"
		"  value      TEXT NOT NULL,"
		"  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP"
		");",
		NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s\n", err);
		sqlite3_free(err);
		return -1;
	}
	if (sqlite3_exec(db,
		"CREATE TABLE IF NOT EXISTS recent_files ("
		"  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  file_path TEXT UNIQUE NOT NULL,"
		"  opened_at INTEGER"
		");",
		NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s\n", err);
		sqlite3_free(err);
		return -1;
	}
	if (sqlite3_exec(db,
		"CREATE TABLE IF NOT EXISTS app_state ("
		"  id                        INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  current_opened            TEXT,"
		"  file_opened_at            INTEGER,"
		"  autosave_interval_seconds INTEGER DEFAULT 60,"
		"  window_width              INTEGER,"
		"  window_height             INTEGER,"
		"  window_x                  INTEGER,"
		"  window_y                  INTEGER,"
		"  autosave_enabled          INTEGER DEFAULT 1"
		");",
		NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s\n", err);
		sqlite3_free(err);
		return -1;
	}
	return 0;
}


static void load_defaults(AppConfig *out)
{
	out->window_width              = 500;
	out->window_height             = 800;
	out->window_x                  = 100;
	out->window_y                  = 100;
	out->autosave_interval_seconds = 5;
	out->auto_save                 = true;
	out->file_opened_at            = 0;
	out->last_opened               = NULL;
	out->edit_font_size            = 16;
	out->ui_scale                  = SCALE_1x;
}

void config_set_value(sqlite3 *db, char *key, char *val)
{
    sqlite3_stmt *stmt;

    sqlite3_prepare_v2(db,
        "INSERT INTO config (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=CURRENT_TIMESTAMP;",
        -1,
        &stmt,
        NULL);

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, val, -1, SQLITE_STATIC);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int config_get_val(sqlite3 *db, const char *key, const char *format_descripter,
		   void *ptr)
{
	sqlite3_stmt *stmt;
	int result = -1;
	sqlite3_prepare_v2(db, "SELECT value FROM config WHERE key = ?;", -1,
			   &stmt, NULL);

	sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

	if (sqlite3_step(stmt) == SQLITE_ROW) {
		result = sscanf((const char*)sqlite3_column_text(stmt, 0),format_descripter,ptr);
	}

	sqlite3_finalize(stmt);
	return result;
}

int init_config(const char *config_path, AppConfig *out)
{
	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	if (!os_exists(config_path) || !os_isfile(config_path)) {
		load_defaults(out);
		return 0;
	}

	sqlite3 *db;
	if (sqlite3_open(config_path, &db) != SQLITE_OK) {
		fprintf(stderr, "%s\n", sqlite3_errmsg(db));
		load_defaults(out);
		return 0;
	}

	if (create_tables(db) != 0) {
		sqlite3_close(db);
		load_defaults(out);
		return 0;
	}

	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db,
		"SELECT current_opened, file_opened_at, autosave_interval_seconds,"
		"       window_width, window_height, window_x, window_y, autosave_enabled"
		" FROM app_state ORDER BY id DESC LIMIT 1;",
		-1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "%s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		load_defaults(out);
		return 0;
	}

	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *s = (const char *)sqlite3_column_text(st, 0);
		if (s) 
			out->last_opened          = strdup(s);
		out->file_opened_at               = sqlite3_column_int64(st, 1);
		out->autosave_interval_seconds    = sqlite3_column_int(st, 2);
		out->window_width                 = sqlite3_column_int(st, 3);
		out->window_height                = sqlite3_column_int(st, 4);
		out->window_x                     = sqlite3_column_int(st, 5);
		out->window_y                     = sqlite3_column_int(st, 6);
		out->auto_save                    = (bool)sqlite3_column_int(st, 7);
	} else {
		// table exits but no row
		load_defaults(out);
	}
	sqlite3_finalize(st);

	if (sqlite3_prepare_v2(db,
		"SELECT file_path FROM recent_files ORDER BY opened_at DESC;",
		-1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "%s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return 0;
	}
	while (sqlite3_step(st) == SQLITE_ROW){
		const char *p = (const char *)sqlite3_column_text(st, 0);
		vpush(out->recent_files, p ? strdup(p) : NULL);
	}
	sqlite3_finalize(st);


	config_get_val(db,"ui_scale","%d",&out->ui_scale);
	config_get_val(db,"edit_font_size","%d",&out->edit_font_size);
	config_get_val(db,"should_wrap","%d",&out->should_wrap);
	out->edit_font_size = MAX(out->edit_font_size,16);
	out->edit_font_size =MIN(out->edit_font_size,EDIT_FONT_SIZE_MAX);
	sqlite3_close(db);
	return 0;
}

void free_cfg(AppConfig* cfg)
{
	if(!cfg->recent_files.items)
		return;
	for(int i = 0 ; i < cfg->recent_files.count;i++){
		free(cfg->recent_files.items[i]);
	}
	cfg->recent_files.count = 0;
}


int save_config(const char *config_path, AppConfig *cfg,bool clear_recent)
{
	if (!cfg)
		return -1;
	glfwGetWindowSize(window, &cfg->window_width, &cfg->window_height);
	glfwGetWindowPos(window, &cfg->window_x, &cfg->window_y);
	sqlite3 *db;
	if (sqlite3_open(config_path, &db) != SQLITE_OK) {
		fprintf(stderr, "%s\n", sqlite3_errmsg(db));
		return -1;
	}

	if (create_tables(db) != 0) {
		sqlite3_close(db);
		return -1;
	}

	char *err = NULL;
	if (sqlite3_exec(db, "DELETE FROM app_state;", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s\n", err);
		sqlite3_free(err);
		sqlite3_close(db);
		return -1;
	}

	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db,
		"INSERT INTO app_state"
		"  (current_opened, file_opened_at, autosave_interval_seconds,"
		"   window_width, window_height, window_x, window_y, autosave_enabled)"
		" VALUES (?,?,?,?,?,?,?,?);",
		-1, &st, NULL) != SQLITE_OK) {
		fprintf(stderr, "%s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return -1;
	}
	sqlite3_bind_text (st, 1, cfg->last_opened, -1, SQLITE_STATIC);
	sqlite3_bind_int64(st, 2, cfg->file_opened_at);
	sqlite3_bind_int  (st, 3, cfg->autosave_interval_seconds);
	sqlite3_bind_int  (st, 4, cfg->window_width);
	sqlite3_bind_int  (st, 5, cfg->window_height);
	sqlite3_bind_int  (st, 6, cfg->window_x);
	sqlite3_bind_int  (st, 7, cfg->window_y);
	sqlite3_bind_int  (st, 8, (int)cfg->auto_save);
	if (sqlite3_step(st) != SQLITE_DONE) {
		fprintf(stderr, "%s\n", sqlite3_errmsg(db));
		sqlite3_finalize(st);
		sqlite3_close(db);
		return -1;
	}
	sqlite3_finalize(st);

	if (cfg->last_opened) {
		if (sqlite3_prepare_v2(db,
			"INSERT OR REPLACE INTO recent_files (file_path, opened_at) VALUES (?, ?);",
			-1, &st, NULL) != SQLITE_OK) {
			fprintf(stderr, "%s\n", sqlite3_errmsg(db));
			sqlite3_close(db);
			return -1;
		}
		sqlite3_bind_text (st, 1, cfg->last_opened, -1, SQLITE_STATIC);
		sqlite3_bind_int64(st, 2, cfg->file_opened_at);
		if (sqlite3_step(st) != SQLITE_DONE)
			fprintf(stderr, "%s\n", sqlite3_errmsg(db));
		sqlite3_finalize(st);
	}
	if(clear_recent){
		sqlite3_exec(db, "DELETE FROM recent_files; DELETE FROM sqlite_sequence WHERE name='recent_files';",
				0, 0, 0);
		free_cfg(cfg);	
	}





	char buffer[100];
	snprintf(buffer,sizeof(buffer),"%d",cfg->edit_font_size);
	config_set_value(db,"edit_font_size",buffer);

	snprintf(buffer,sizeof(buffer),"%d",cfg->ui_scale);
	config_set_value(db,"ui_scale",buffer);

	snprintf(buffer,sizeof(buffer),"%d",cfg->should_wrap);
	config_set_value(db,"should_wrap",buffer);

	sqlite3_close(db);
	return 0;
}

bool jh_radio_button(const char *label, int *v, int id, ImVec2 size)
{
	bool is_selected = (*v == id);

	if (is_selected) {
		const ImVec4 *pressed_color =
		    igGetStyleColorVec4(ImGuiCol_ButtonActive);
		igPushStyleColor_Vec4(ImGuiCol_Button, *pressed_color);
	}

	bool clicked = igButton(label, size);

	if (is_selected) {
		igPopStyleColor(1);
	}

	if (clicked) {
		*v = id;
		if(!is_selected)
			return true;
		return false;
	}

	return false;
}

bool welcome_screen(char *current_file, int n)
{
	igBeginGroup();
	bool result = false;
	igText(i_file_text2);
	igSameLine(0.0f, -1.0f);
	if (igTextLink("Create Notes")) {
		time_t now = time(NULL);
		struct tm *t = localtime(&now);

		char default_filename[256];
		strftime(default_filename, sizeof(default_filename),
			 "%Y-%m-%d_%H.%M.%S_untitle.jnt", t);

		const char *savefile = tinyfd_saveFileDialog(
		    "Save File", default_filename, 0, NULL, NULL);

		if (savefile) {
			if(os_exists(savefile)){
				os_unlink(savefile);
			}
			size_t len = strlen(savefile);
			size_t ext_len = sizeof(FILE_EXT) - 1;
			int has_ext =
			    len > ext_len &&
			    strcmp(savefile + len - ext_len, FILE_EXT) == 0;

			strncpy(current_file, savefile, n - 1);
			current_file[n - 1] = '\0';

			if (!has_ext) {
				size_t cur_len = strlen(current_file);
				if (cur_len + ext_len < (size_t)n) {
					strncat(current_file, FILE_EXT,
						n - cur_len - 1);
				}
			}
			result = true;
		}
	}

	igText(i_folder_open);
	igSameLine(0.0f, -1.0f);
	if (igTextLink("Open Existing")) {
		const char *filters[] = {"*" FILE_EXT};

		const char *open_file = tinyfd_openFileDialog(
		    "Open", "", sizeof(filters) / sizeof(filters[0]), filters,
		    NULL, 0);

		if (open_file) {
			strncpy(current_file, open_file, n - 1);
			current_file[n - 1] = '\0';
			result = true;
		}
	}
	
	igDummy((ImVec2){0,20});
	igText("Settings");
	igSeparator();

	igText("UI Scale");
	igSpacing();

	const int item_count = 4;
	const float total_width = igGetContentRegionAvail().x;
	const float spacing = igGetStyle()->ItemSpacing.x;
	const float cell = (total_width - (item_count - 1) * spacing) / item_count;
	ImVec2 btn_size = {cell,0};

	if(jh_radio_button("1",&scale_idx,0,btn_size)){
		igGetStyle()->FontScaleMain = 1;
		app_cfg.ui_scale = SCALE_1x;
	}
	igSameLine(0.0f, -1.0f);

	if(jh_radio_button("1.25",&scale_idx,1,btn_size)){
		igGetStyle()->FontScaleMain = 1.25;
		app_cfg.ui_scale = SCALE_1_25x;
	}
	igSameLine(0.0f, -1.0f);


	if(jh_radio_button("1.5",&scale_idx,2,btn_size)){
		igGetStyle()->FontScaleMain = 1.5;
		app_cfg.ui_scale = SCALE_1_5x;
	}

	igSameLine(0.0f, -1.0f);

	if(jh_radio_button("2",&scale_idx,3,btn_size)){
		igGetStyle()->FontScaleMain = 2;
		app_cfg.ui_scale = SCALE_2x;
	}


	igDummy((ImVec2){0, 100});
	igBeginChild_Str("##recentlist", (ImVec2){-FLT_MIN, 300}, 0, 0);
	igText("Recent");
	igSameLine(0.0f, -1.0f);
	igText(i_fire);
	igSameLine(0.0f, 0.0f);
	if(igTextLink("Clear Recent")){
		save_config(config_file,&app_cfg,1);
	}
	igSeparator();
	ImGuiListClipper clipper;
	ImGuiListClipper_Begin(&clipper, app_cfg.recent_files.count,
			igGetTextLineHeightWithSpacing());

	while (ImGuiListClipper_Step(&clipper)) {
	    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
		igPushID_Int(i);
		
		if (igTextLink(os_path_basename(app_cfg.recent_files.items[i]))) {
		    strncpy(current_file, app_cfg.recent_files.items[i], n);
		    current_file[n - 1] = '\0';
		    result = true;
		    focus_once = true;
		}
		
		if (igBeginPopupContextItem("context_menue", ImGuiPopupFlags_MouseButtonRight)) {
		    if (igMenuItem_Bool("Copy Path", NULL, false, true)) {
			igSetClipboardText(app_cfg.recent_files.items[i]);
		    }
		    if (igMenuItem_Bool("Open Folder", NULL, false, true)) {
		    }
		    igEndPopup();
		}
		
		igPopID();
	    }
	}
ImGuiListClipper_End(&clipper);

	igEndChild();

	igEndGroup();
	return result;
}


static void page_build_title(Page *page)
{
	int i = 0;
        for (i = 0; page->content[i] && i < sizeof(page->title) - 1; i++) {
                page->title[i] = page->content[i];
                if (page->title[i] == '\n' || page->title[i] == '\r')
                        page->title[i] = ' ';
        }
        page->title[i] = '\0';
}

static void back_action(Notebook *nb, Page *page, int mode)
{
        curr_screen = SCREEN_MAIN;

        if (!page->title[0] && !page->content[0])
                return;

        if (page->title[0] == '\0')
                page_build_title(page);

        if (mode == 0)
                notes_add(nb, page);
        else
                notes_save(nb, page);
}




void edit_widget(Notebook *nb, Page *page)
{

	igBeginChild_Str("##PageEdit", (ImVec2){0, 0},0, 0);
	
	ImVec4 transparent_bg = { 0.0f, 0.0f, 0.0f, 0.0f }; // Completely clear
	igPushStyleColor_Vec4(ImGuiCol_FrameBg, transparent_bg);
	igSetNextItemWidth(-FLT_MIN);
	igPushFont(NULL,app_cfg.edit_font_size);
	igInputTextWithHint("##title", "Title", page->title,
				sizeof(page->title), 0, NULL, NULL);
	igSeparator();
	if(igInputTextMultiline("##content", page->content,
			     page->content_len, (ImVec2){-FLT_MIN, -FLT_MIN},
			     ImGuiInputTextFlags_AllowTabInput|((app_cfg.should_wrap)?ImGuiInputTextFlags_WordWrap:0),
			      NULL, NULL)){
	}
	igPopFont();
	igPopStyleColor(1);
	igEndChild();
}

void render_image_list(Notebook *nb, Page *page, int mode)
{
}


void render_attachment_list(Notebook *nb, Page *page, int mode)
{
}

//TODO: handle error;
void page_edit_screen(Notebook *nb, Page *page, int mode) // 0:add ,1:edit
{
	if (!page) {

		igBeginChild_Str("##PageEdit", (ImVec2){0, 0}, 0, 0);
		igText("Seomthing Wrong");
		igEndChild();
		return;
	}


	enum { JH_EDIT, JH_IMAGES, JH_ATTACHMENTS, MODE_MAX } JH_MODE;
	igBeginGroup();
	static int selected = JH_EDIT;
	if (igSmallButton(i_arrow_left2))
		back_action(nb, page, mode);
#if 0

	igSameLine(0.0f, -1.0f);
	if (jh_radio_smallbutton(i_pencil2, &selected, JH_EDIT)) {
	}

	igSameLine(0.0f, -1.0f);
	if (jh_radio_smallbutton(i_images, &selected, JH_ATTACHMENTS)) {
	}

	igSameLine(0.0f, -1.0f);
	if (jh_radio_smallbutton(i_attachment, &selected, JH_IMAGES)) {
	}
#endif
switch(selected){
	case JH_IMAGES:
		igEndGroup();
		render_image_list(nb,page,mode);
		break;
	case JH_ATTACHMENTS:
		render_attachment_list(nb,page,mode);
		igEndGroup();
		break;
	default:


	igSameLine(0.0f, -1.0f);
	if (igSmallButton(i_zoom_out)) {
		if (app_cfg.edit_font_size > 8)
			app_cfg.edit_font_size--;
	}

	igSameLine(0.0f, -1.0f);
	if (igSmallButton(i_zoom_in)) {
		if (app_cfg.edit_font_size < EDIT_FONT_SIZE_MAX)
			app_cfg.edit_font_size++;
	}

	/* igSameLine(0.0f, -1.0f); */
	/* if (igSmallButton(i_plus)) { */
	/* } */

	igSameLine(0.0f, -1.0f);
	if (igSmallButton(app_cfg.should_wrap ? "Wrap:ON" : "Wrap:OFF"))
		app_cfg.should_wrap = !app_cfg.should_wrap;
	igEndGroup();
	edit_widget(nb, page);
}

	/* if (igSelectable_Bool("Search",true, 0, (ImVec2){100,0})){}
	 */
}

bool jh_icon_input_text_with_clear(const char *icon, const char *clear_icon,
				   const char *label, const char *hint,
				   char *buf, size_t buf_size,
				   ImGuiInputTextFlags flags,
				   ImGuiInputTextCallback callback,
				   void *user_data)
{
	ImGuiStyle *style = igGetStyle();
	const ImVec4 transparent = (ImVec4){0, 0, 0, 0};
	igPushID_Str(label);
	igBeginGroup();

	ImGuiStorage *st = igGetStateStorage();
	ImGuiID refocus_id = igGetID_Str("refocus");
	ImGuiID clrstate_id = igGetID_Str("clearstate");

	float total_w =
	    igCalcItemWidth(); // honors caller's SetNextItemWidth
	float frame_h = igGetFrameHeight();
	ImVec2 pos = igGetCursorScreenPos();

	// one background for the whole widget
	ImDrawList *dl = igGetWindowDrawList();
	ImDrawList_AddRectFilled(
	    dl, pos, (ImVec2){pos.x + total_w, pos.y + frame_h},
	    igGetColorU32_Col(ImGuiCol_FrameBg, 1.0f), style->FrameRounding, 0);
	if (style->FrameBorderSize > 0.0f)
		ImDrawList_AddRect(
		    dl, pos, (ImVec2){pos.x + total_w, pos.y + frame_h},
		    igGetColorU32_Col(ImGuiCol_Border, 1.0f),
		    style->FrameRounding, 0, style->FrameBorderSize);

	// segments draw NO background
	igPushStyleColor_Vec4(ImGuiCol_FrameBg, transparent);
	igPushStyleColor_Vec4(ImGuiCol_Button, transparent);
	igPushStyleColor_Vec4(ImGuiCol_ButtonHovered, transparent);
	igPushStyleColor_Vec4(ImGuiCol_ButtonActive, transparent);
	igPushStyleVar_Float(ImGuiStyleVar_FrameBorderSize, 0.0f);

	// left icon segment 
	igPushStyleColor_Vec4(ImGuiCol_Text,
			      *igGetStyleColorVec4(ImGuiCol_TextDisabled));
	igButton(icon, (ImVec2){0, 0});
	igPopStyleColor(1);
	float left_w = igGetItemRectSize().x;

	igSameLine(0.0f, 0.0f);

	//middle: the input
	bool show_clear = clear_icon && buf[0] != '\0';
	float right_w = 0.0f;
	if (show_clear) {
		ImVec2 cs = igCalcTextSize(clear_icon, NULL, false, -1.0f);
		right_w = cs.x + style->FramePadding.x * 2.0f;
	}

	if (ImGuiStorage_GetBool(st, refocus_id, false)) {
		igSetKeyboardFocusHere(0);
		ImGuiStorage_SetBool(st, refocus_id, false);
	}

	igSetNextItemWidth(total_w - left_w - right_w);
	igPushStyleColor_Vec4(ImGuiCol_NavCursor, transparent);
	bool text_changed = igInputTextWithHint(label, hint, buf, buf_size, flags, callback,
			    user_data);
	igPopStyleColor(1);

	//right clear with hover/press feedback
	if (show_clear) {
		igSameLine(0.0f, 0.0f);

		int cstate = ImGuiStorage_GetInt(st, clrstate_id, 0);
		ImVec4 tcol = *igGetStyleColorVec4(
		    cstate == 0 ? ImGuiCol_TextDisabled : ImGuiCol_Text);
		if (cstate == 2) {
			tcol.x *= 0.75f;
			tcol.y *= 0.75f;
			tcol.z *= 0.75f;
		}

		igPushStyleColor_Vec4(ImGuiCol_Text, tcol);
		if (igButton(clear_icon, (ImVec2){right_w, 0})) {
			buf[0] = '\0';
			ImGuiStorage_SetBool(st, refocus_id, true);
		}
		igPopStyleColor(1);

		ImGuiStorage_SetInt(st, clrstate_id,
				    igIsItemActive()	 ? 2
				    : igIsItemHovered(0) ? 1
							 : 0);
	}

	igPopStyleVar(1);   // FrameBorderSize
	igPopStyleColor(4); // FrameBg + 3 button colors

	igEndGroup();
	igPopID();
	return text_changed;
}


void jh_icon_input_text(const char *icon, const char *label, const char *hint,
                        char *buf, size_t buf_size, ImGuiInputTextFlags flags,
                        ImGuiInputTextCallback callback, void *user_data)
{
    ImGuiStyle *style = igGetStyle();

    const float pad_x = style->FramePadding.x;      // frame edge -> icon
    const float gap   = style->ItemInnerSpacing.x;  // icon -> text

    ImVec2 icon_size = igCalcTextSize(icon, NULL, false, -1.0f);
    ImVec2 pos       = igGetCursorScreenPos();      // frame top-left

    // reserve room on the left of the frame for the icon
    igPushStyleVar_Vec2(ImGuiStyleVar_FramePadding,
                        (ImVec2){ pad_x + icon_size.x + gap,
                                  style->FramePadding.y });

    igInputTextWithHint(label, hint, buf, buf_size, flags, callback, user_data);

    igPopStyleVar(1);

    // paint icon inside the frame, vertically centered
    ImDrawList *dl  = igGetWindowDrawList();
    float frame_h   = igGetFrameHeight();
    ImVec2 icon_pos = {
        pos.x + pad_x,
        pos.y + (frame_h - icon_size.y) * 0.5f
    };
    ImDrawList_AddText_Vec2(dl, icon_pos,
                            igGetColorU32_Col(ImGuiCol_TextDisabled, 1.0f),
                            icon, NULL);
}



void show_popup(const char *title, const char *message, void *ud)
{
    static char  s_title[128];
    static char  s_msg[512];
    static void *s_ud      = NULL;
    static bool  s_pending = false;
    static bool  s_active  = false;

    // ---------- request mode ---------- 
    if (title) {
        snprintf(s_title, sizeof s_title, "%s###app_popup", title);
        snprintf(s_msg, sizeof s_msg, "%s", message ? message : "");
        s_ud      = ud;
        s_pending = true;
        return;
    }

    if (s_pending) {
        igOpenPopup_Str(s_title, 0);
        s_pending = false;
        s_active  = true;
    }
    if (!s_active)
        return;

    ImVec2 center = ImGuiViewport_GetCenter(igGetMainViewport());
    igSetNextWindowPos(center, ImGuiCond_Appearing, (ImVec2){0.5f, 0.5f}); 
    if (igBeginPopupModal(s_title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        /* igTextWrapped("%s", s_msg); */
        igText("%s", s_msg);
        igSeparator();

        if (igButton("OK", (ImVec2){120, 0})) {
            igCloseCurrentPopup();
            s_active = false;
        }

        igEndPopup();
    }

    (void)s_ud;
}

void cleanup(BackendDescriptor*bck_desc,Notebook* notebook)
{

	for(int i=0;i<bck_desc->count ;i++)
		free(bck_desc->fields[i].value);
	notes_free(notebook);
	notes_free_descriptor(bck_desc);
}

void close_app(void)
{
	if (curr_screen == SCREEN_EDIT) {
		if (edit_mode == 1)
			notes_save(&notebook, &current_page);
	}
	if (current_file[0] != '\0' && os_isfile(current_file)) {
		app_cfg.last_opened = current_file;
		app_cfg.file_opened_at = time(NULL);
	}
	save_config(config_file, &app_cfg, 0);
	cleanup(&bck_des, &notebook);
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	igDestroyContext(NULL);

	glfwDestroyWindow(window);
	glfwTerminate();

	exit(0);
}

static void go_home(CurrenScreen *screen, const char *current_file, char *search_buffer)
{
	*screen = SCREEN_WELCOME;
	app_cfg.last_opened = current_file;
	save_config(config_file, &app_cfg, 0);
	free_cfg(&app_cfg);
	init_config(config_file, &app_cfg);
	if (search_buffer)
		search_buffer[0] = '\0';
}

static void render_welcome_screen(char *current_file, int current_file_size,
				   CurrenScreen *screen, BackendDescriptor *bck_des,
				   Notebook *notebook)
{
	if (welcome_screen(current_file, current_file_size)) {
		*screen = SCREEN_INIT;
		cleanup(bck_des, notebook);
		notes_describe(current_file, bck_des);
	}
}

static void render_field_inputs(BackendDescriptor *bck_des, bool show_error, bool *fullfiled)
{
	for (int i = 0; i < bck_des->count; i++) {
		Field *fld = &bck_des->fields[i];
		if (fld->value_max_length < 1) {
			fld->value = NULL;
			continue;
		}
		if (!fld->value)
			fld->value = calloc(fld->value_max_length + 1, sizeof(*fld->value));

		igPushID_Int(i);
		igPushItemWidth(-FLT_MIN);

		if (focus_once) {
			igSetKeyboardFocusHere(1);
			focus_once = false;
		}
		if (fld->secret)
			jh_icon_input_text_with_clear(i_lock, i_cross, "##input_pass", fld->label,
						       fld->value, fld->value_max_length,
						       ImGuiInputTextFlags_Password, NULL, NULL);
		else
			jh_icon_input_text_with_clear(i_lock, i_cross, "##input", fld->label,
						       fld->value, fld->value_max_length, 0, NULL,
						       NULL);
		igPopItemWidth();
		if (fld->required && fld->value[0] != '\0')
			*fullfiled = true;
		if (show_error && fld->required && fld->value[0] == '\0')
			igText("This is required field ");
		igPopID();
	}
}

static void render_init_screen(char *current_file, CurrenScreen *screen,
				BackendDescriptor *bck_des, Notebook *notebook)
{
	if (igSmallButton(i_home"##init"))
		go_home(screen, current_file, NULL);

	const int text_box_height = 50;
	ImVec2 avail = igGetContentRegionAvail();
	igSetCursorPosY((avail.y - text_box_height * bck_des->count) * 0.5f);

	static bool show_error = false;
	static bool fullfiled = false;

	render_field_inputs(bck_des, show_error, &fullfiled);

	if (igButton(bck_des->btn_text, (ImVec2){-FLT_MIN, 0})) {
		if (!fullfiled) {
			show_error = true;
		} else if (notes_init(current_file, bck_des, notebook) >= 0) {
			*screen = SCREEN_MAIN;
		}
	}
}

static void start_new_page(Page *current_page, CurrenScreen *screen, int *edit_mode)
{
	*screen = SCREEN_EDIT;
	const int content_len = 1 << 16;
	current_page->title[0] = '\0';
	if (current_page->content_len >= content_len) {
		current_page->content[0] = '\0';
	} else {
		current_page->content = calloc(content_len, sizeof(*current_page->content));
		if (!current_page->content) {
			fprintf(stderr, "Error: Unable to allocate memory.\nExiting\n");
			close_app();
		}
		current_page->content[0] = '\0';
		current_page->content_len = content_len;
	}
	*edit_mode = 0;
}

static bool render_delete_confirm_popup(Notebook *notebook, int idx, int display_start,
					 int *pending_delete)
{
	if (!igBeginPopupModal("Confirm", NULL, ImGuiWindowFlags_AlwaysAutoResize))
		return false;

	int result = 0;
	igText("Do you want to delete it?");

	if (igButton("OK", (ImVec2){120, 0})) {
		result = 1;
		igCloseCurrentPopup();
	}
	igSameLine(0, -1);
	if (igButton("Cancel", (ImVec2){120, 0})) {
		result = 2;
		igCloseCurrentPopup();
	}
	igEndPopup();

	if (result == 1) {
		notes_remove(notebook, idx);
		int total_pages = notes_get_total(notebook);
		notes_update_visible_view(notebook, display_start, total_pages);
		*pending_delete = -1;
		return true;
	}
	if (result == 2)
		*pending_delete = -1;
	return false;
}

static bool open_page_for_edit(Notebook *notebook, int idx, Page *current_page, int *edit_mode,
				CurrenScreen *screen)
{
	Page *p = notes_get(notebook, idx);
	if (!p)
		return false;

	int len = p->content_len * 2;
	len = MAX(len,MIN_CONTENT_BUFFER);
	if (!current_page->content || current_page->content_len < len) {
		free(current_page->content);
		current_page->content = calloc(len, sizeof(*current_page->content));
		if (!current_page->content) {
			fprintf(stderr, "Error: Unable to allocate memory.\nExiting\n");
			close_app();
		}
		current_page->content_len = len;
	}
	memset(current_page->content,0,current_page->content_len);
	memcpy(current_page->content, p->content, p->content_len);
	memcpy(current_page->title, p->title, sizeof(p->title));
	current_page->key = p->key;
	current_page->id = p->id;
	*edit_mode = 1;
	*screen = SCREEN_EDIT;
	return true;
}

static bool render_note_list_row(Notebook *notebook, int idx, int display_start,
				  Page *current_page, int *edit_mode, CurrenScreen *screen)
{
	static int pending_delete = -1;
	bool jump_to_edit = false;

	const char *present = notes_get_presentation(notebook, idx);
	if (!present)
		return false;

	igPushID_Int(idx + 1);
	if (igSmallButton(i_bin)) {
		pending_delete = idx;
		igOpenPopup_Str("Confirm", 0);
	}
	if (pending_delete == idx) {
		if (render_delete_confirm_popup(notebook, idx, display_start, &pending_delete)) {
			igPopID();
			return false;
		}
	}
	igPopID();

	igPushID_Int(idx + 2);
	igSameLine(0.0f, -1.0f);
	if (igTextLink(present)) {
		jump_to_edit = open_page_for_edit(notebook, idx, current_page, edit_mode, screen);
	}
	igPopID();

	return jump_to_edit;
}

static void render_note_list(Notebook *notebook, Page *current_page, int *edit_mode,
			      CurrenScreen *screen)
{
	igBeginChild_Str("list", (ImVec2){0, 0}, 0, 0);

	ImGuiListClipper clipper;
	ImGuiListClipper_Begin(&clipper, notes_get_total(notebook), igGetTextLineHeightWithSpacing());

	bool jump_to_edit = false;
	while (!jump_to_edit && ImGuiListClipper_Step(&clipper)) {
		notes_update_visible_view(notebook, clipper.DisplayStart, clipper.DisplayEnd);

		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
			if (render_note_list_row(notebook, i, clipper.DisplayStart, current_page,
						  edit_mode, screen)) {
				jump_to_edit = true;
				break;
			}
		}
	}

	ImGuiListClipper_End(&clipper);
	igEndChild();
}

static void render_main_screen(char *current_file, CurrenScreen *screen, Notebook *notebook,
				Page *current_page, int *edit_mode)
{
	static char search_buffer[TITLE_MAX] = {0};

	if (igSmallButton(i_home))
		go_home(screen, current_file, search_buffer);
	igSameLine(0.0f, -1.0f);

	if (igSmallButton(i_file_text2))
		start_new_page(current_page, screen, edit_mode);

	igSpacing();
	igSetNextItemWidth(-FLT_MIN);
	if (jh_icon_input_text_with_clear(i_search, i_cross, "##search", "Search", search_buffer,
					   sizeof(search_buffer), 0, NULL, NULL)) {
		notes_set_search_query(notebook, search_buffer);
	}

	render_note_list(notebook, current_page, edit_mode, screen);
}

static void render_frame(char *current_file, int current_file_size, CurrenScreen *screen,
			  BackendDescriptor *bck_des, Notebook *notebook, Page *current_page,
			  int *edit_mode)
{
	//  motion_keys_update(1.0f);

	ImGuiViewport *viewport = igGetMainViewport();
	igSetNextWindowPos(viewport->Pos, ImGuiCond_Always, (ImVec2){0, 0});
	igSetNextWindowSize(viewport->Size, ImGuiCond_Always);

	igBegin("main_window", NULL,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
		    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

	switch (*screen) {
	case SCREEN_WELCOME:
		render_welcome_screen(current_file, current_file_size, screen, bck_des, notebook);
		break;
	case SCREEN_INIT:
		render_init_screen(current_file, screen, bck_des, notebook);
		break;
	case SCREEN_MAIN:
		render_main_screen(current_file, screen, notebook, current_page, edit_mode);
		break;
	case SCREEN_EDIT:
		page_edit_screen(notebook, current_page, *edit_mode);
		break;
	}

	show_popup(NULL, NULL, NULL);
	igEnd();
}

static void render_gl(ImVec4 clearColor)
{
	igRender();
	glfwMakeContextCurrent(window);
	glViewport(0, 0, (int)ioptr->DisplaySize.x, (int)ioptr->DisplaySize.y);
	glClearColor(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
	glfwSwapBuffers(window);
}

int main(int argc, char *argv[])
{

	bool load_config = true;

	memset(&bck_des,0,sizeof(bck_des));
	memset(&notebook,0,sizeof(notebook));
	notebook.create_popup = show_popup;

	os_path_join(config_folder,sizeof(config_folder),os_configdir(),JH_APP_NAME);
	os_path_join(config_file,sizeof(config_file),config_folder,JH_CONFIG_FILE);
	if(!os_exists(config_folder)){
		os_makedirs(config_folder);
		load_config = os_exists(config_folder);
	}	

	if(load_config){
		init_config(config_file,&app_cfg);
		scale_idx = app_cfg.ui_scale;	
	}
	if(app_cfg.last_opened && os_isfile(app_cfg.last_opened)){
		snprintf(current_file, sizeof(current_file),"%s",app_cfg.last_opened);
		notes_describe(current_file, &bck_des);//TODO: handle error
		/* show_init_screen = true; */
		curr_screen = SCREEN_INIT;
		/* curr_screen = SCREEN_WELCOME; */
	}else{
		curr_screen = SCREEN_WELCOME;
	}
	print_app_cfg(&app_cfg);


	if (!glfwInit())
		return -1;

	// Decide GL+GLSL versions
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);

#if __APPLE__
	// GL 3.2 Core + GLSL 150
	const char *glsl_version = "#version 150";
#else
	// GL 3.2 + GLSL 130
	const char *glsl_version = "#version 130";
#endif

	// just an extra window hint for resize
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(
	    glfwGetPrimaryMonitor()); // Valid on GLFW 3.3+ only
	window =
	    glfwCreateWindow(app_cfg.window_width, app_cfg.window_height,JH_APP_NAME,NULL, NULL);

	if (!window) {
		printf("Failed to create window! Terminating!\n");
		glfwTerminate();
		return -1;
	}

	glfwSetWindowPos(window, app_cfg.window_x,app_cfg.window_y);
	glfwMakeContextCurrent(window);
	gladLoadGL(glfwGetProcAddress);

	// enable vsync
	glfwSwapInterval(1);

	// check opengl version sdl uses
	printf("opengl version: %s\n", (char *)glGetString(GL_VERSION));

	// setup imgui
	igCreateContext(NULL);

	// set docking
	ioptr = igGetIO();
	ioptr->ConfigFlags |=
	    ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
	// ioptr->ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable
	// Gamepad Controls
	/* #ifdef IMGUI_HAS_DOCK */
	/* 	ioptr->ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable
	 * Docking */
	/* 	ioptr->ConfigFlags |= */
	/* 	    ImGuiConfigFlags_ViewportsEnable; // Enable Multi-Viewport /
	 */
	/* 					      // Platform Windows */
	/* #endif */

	// Setup scaling
	ImGuiStyle *style = igGetStyle();
	ImGuiStyle_ScaleAllSizes(
	    style,
	    main_scale); // Bake a fixed style scale. (until we have a solution
			 // for dynamic style scaling, changing this requires
			 // resetting Style + calling this again)
	style->FontScaleDpi =
	    main_scale; // Set initial font scale. (using
			// io.ConfigDpiScaleFonts=true makes this unnecessary.
			// We leave both here for documentation purpose)
#if GLFW_VERSION_MAJOR >= 3 && GLFW_VERSION_MINOR >= 3
	ioptr->ConfigDpiScaleFonts =
	    true; // [Experimental] Automatically overwrite style.FontScaleDpi
		  // in Begin() when Monitor DPI changes. This will scale fonts
		  // but _NOT_ scale sizes/padding for now.
	ioptr->ConfigDpiScaleViewports =
	    true; // [Experimental] Scale Dear ImGui and Platform Windows when
		  // Monitor DPI changes.
#endif
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	igStyleColorsDark(NULL);

style->WindowRounding  = 0.0f;
style->FrameRounding   = 10.0f;
style->GrabRounding    = 10.0f;
style->TabRounding     = 10.0f;
style->ScrollbarSize   = 10.0f;

/* style->HoverDelayShort = 0.0f; */
/* style->HoverDelayNormal = 0.0f; */
/* style->HoverStationaryDelay = 0.0f; */
/* 	ioptr->MouseDoubleClickTime = 0.0f;*/



	// ImFontAtlas_AddFontDefault(io.Fonts, NULL);
const float ui_font_size   = 16.0f;
const float icon_font_size = ui_font_size * 0.75f;  /* relative; tune 0.7–0.8 */

ImFontConfig *font_cfg = ImFontConfig_ImFontConfig();
font_cfg->SizePixels = ui_font_size;
ImFontAtlas_AddFontDefault(ioptr->Fonts, font_cfg);
ImFontConfig_destroy(font_cfg);

ImFontConfig *icon_cfg = ImFontConfig_ImFontConfig();
icon_cfg->MergeMode        = true;
icon_cfg->PixelSnapH       = true;
icon_cfg->GlyphOffset.y    = 1.0f;            /* re-tune; rounding is automatic */
icon_cfg->GlyphMinAdvanceX = icon_font_size;
icon_cfg->FontDataOwnedByAtlas = false;

icon_font = ImFontAtlas_AddFontFromMemoryTTF(ioptr->Fonts, icomoon_ttf, icomoon_ttf_len, icon_font_size,icon_cfg, NULL);
/* icon_font = ImFontAtlas_AddFontFromFileTTF(ioptr->Fonts, "icomoon.ttf", icon_font_size, icon_cfg, NULL); */
ImFontConfig_destroy(icon_cfg);



	ImVec4 clearColor;
	clearColor.x = 0.00f;
	clearColor.y = 0.00f;
	clearColor.z = 0.00f;
	clearColor.w = 0.00f;
	style->Colors[ImGuiCol_WindowBg] = clearColor;

	// main event loop

	switch(app_cfg.ui_scale)
	{
		case SCALE_1x:
			style->FontScaleMain = 1.0f;
			break;

		case SCALE_1_25x:
			style->FontScaleMain = 1.25f;
			break;

		case SCALE_1_5x:
			style->FontScaleMain = 1.5f;
			break;

		case SCALE_2x:
			style->FontScaleMain = 2.0f;
			break;
		default:
			style->FontScaleMain = 1.0f;
	}

	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		igNewFrame();

		render_frame(current_file, sizeof(current_file), &curr_screen, &bck_des, &notebook,
			     &current_page, &edit_mode);

		render_gl(clearColor);
	}

	close_app();
	return 0;
}
