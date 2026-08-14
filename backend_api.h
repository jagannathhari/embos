#ifndef BACKEND_API_H
#define BACKEND_API_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_FILEDS (1 << 3)   
#define TITLE_MAX (1<<8)

typedef struct {
    char  *key;           // machine key, e.g. "password"
    char  *label;         // human-readable prompt shown in UI
    char  *value;         // filled in by the UI before notes_init()
    void (*on_change)(const char *value, void *user_data);
    void  *user_data;
    bool   required;
    bool   secret;        // mask input in the UI
    int    value_max_length;  // max chars the UI should allow
    int    value_min_lenght;
} Field;

typedef struct {
    Field fields[MAX_FILEDS];
    const char* btn_text;
    void* ctx;
    int   count;
} BackendDescriptor;


//required id or key
typedef struct {
    char    title[TITLE_MAX]; // max 255 chars
    char    *content;
    int content_len;
    char* key; 
    int id;
    uint64_t created_on;
    uint64_t modified_on;
} Page;

typedef struct {
    Page* pages;
    void* ctx; //for the backend
    char* uri;    
    char* author;
    char* notebook_name;
    void (*create_popup)(const char*,const char *, void *);//populated by frontend
    void* user_data;// populated by frontend
    uint64_t created_on;
    uint64_t modified_on;
} Notebook;

int notes_describe(const char *path, BackendDescriptor *out);

// create_popup(const char* title,const char*msg,void* ud)
int notes_init(const char *path, BackendDescriptor *desc, Notebook *out);
int notes_get_total(Notebook *nb);
void notes_update_visible_view(Notebook* nb,int start_idx, int end_idx);
const char* notes_get_presentation(Notebook *nb, int i);
Page *notes_get(Notebook *nb, int i);
int notes_save(Notebook *nb, Page* page_id);
int notes_add(Notebook *nb, Page *page);
int notes_remove(Notebook *nb, int i);
void notes_free(Notebook *nb);
void notes_free_descriptor(BackendDescriptor *desc);

#endif // BACKEND_API_H
