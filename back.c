#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "sqlite3.h"
#include "backend_api.h"
#include "os.h"

#define MAX_PASSWORD_LENGHT (1 << 4)
#define MAX_AUTHOR_NAME (1 << 8)
#define MAX_CACHE_SIZE (1<<10)
typedef struct {
	sqlite3 *db;
	Page pages[MAX_CACHE_SIZE];
	int total_pages;
	int offset;//starting index
	bool create_file;
	bool is_searching;
	bool use_like_fallback;
	char search_query[TITLE_MAX];
	int search_total;
} Ctx;


static void default_error_callback(const char *title, const char *msg, void *ud)
{
	fprintf(stderr, "[%s]:%s\n", title, msg);
}

static int notes_create_notebook(BackendDescriptor *out)
{
	int total_fields = 0;
	out->fields[total_fields++] = (Field){
	    .key = "password",
	    .label = "Enter password",
	    .required = true,
	    .secret = true,
	    .value_max_length = MAX_PASSWORD_LENGHT,

	};

	out->fields[total_fields++] = (Field){
	    .key = "author",
	    .label = "Author Name",
	    .required = false,
	    .secret = false,
	    .value_max_length = MAX_AUTHOR_NAME,

	};
	out->btn_text = "Create";
	out->count = total_fields;

	return 0;
}

int notes_describe(const char *path, BackendDescriptor *out)
{
	if (!out)
		return -1;
	Ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;
	ctx->offset = -1;
	if (!os_exists(path)) {
		int result = notes_create_notebook(out);
		if (result < 0) {
			free(ctx);
			return -1;
		}
		ctx->create_file = true;
		out->ctx = ctx;
		return 0;
	}

	if (!os_isfile(path)) {
		free(ctx);
		return -1;
	}

	int total_fields = 0;
	out->fields[total_fields++] = (Field){
	    .key = "password",
	    .label = "Enter password",
	    .required = true,
	    .secret = true,
	    .value_max_length = MAX_PASSWORD_LENGHT,

	};
	out->count = total_fields;
	ctx->create_file = false;
	ctx->is_searching = false;
	out->ctx = ctx;
	out->btn_text = "Login";
	return 0;
}

int notes_get_total(Notebook *nb)
{
	Ctx *ctx = nb->ctx;
	return ctx->is_searching ? ctx->search_total : ctx->total_pages;
}

int notes_init(const char *path, BackendDescriptor *desc, Notebook *out)
{
    if (!out) {
        return -1;
    }

    void (*create_popup)(const char *, const char *, void *) =
        default_error_callback;
    void *user_data = NULL;

    if (out->create_popup) {
        create_popup = out->create_popup;
        user_data = out->user_data;
    }

    if (!desc) {
        create_popup("Error", "BackendDescriptor not found", user_data);
        return -1;
    }

    Ctx *ctx = desc->ctx;

    if (desc->count < 1 || desc->count >= MAX_FILEDS) {
        create_popup("Error", "Internal Error Occur", user_data);
        return -1;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);

    if (rc != SQLITE_OK) {
        create_popup("File open Failed",
                     db ? sqlite3_errmsg(db) : "unknown error",
                     user_data);
        if (db) sqlite3_close(db);
        return -1;
    }

    char *password = NULL;
    char *author = NULL;

    for (int i = 0; i < desc->count; i++) {
        if (desc->fields[i].key && strcmp(desc->fields[i].key, "password") == 0) {
            password = desc->fields[i].value;
        } else if (desc->fields[i].key && strcmp(desc->fields[i].key, "author") == 0) {
            author = desc->fields[i].value;
        }
    }

    if (!password) {
        create_popup("Error", "Password missing", user_data);
        sqlite3_close(db);
        return -1;
    }

    int pass_len = strlen(password);

    if (pass_len <= 0) {
        create_popup("Error", "Empty password", user_data);
        sqlite3_close(db);
        return -1;
    }

    rc = sqlite3_key(db, password, pass_len);

    if (rc != SQLITE_OK) {
        create_popup("Wrong Password",
                     db ? sqlite3_errmsg(db) : "unknown error",
                     user_data);
        sqlite3_close(db);
        return -1;
    }

    if (!author) {
        author = "Unknown";
    }

    const char *table_metadata =
        "CREATE TABLE IF NOT EXISTS metadata ("
        "id INTEGER PRIMARY KEY CHECK (id = 1),"
        "author TEXT,"
        "version TEXT,"
        "modified_on INTEGER,"
        "created_on INTEGER,"
        "comment TEXT,"
        "theme TEXT"
        ");";

    const char *table_page =
        "CREATE TABLE IF NOT EXISTS page ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "title VARCHAR(1024),"
        "content TEXT,"
    	"is_pinned INTEGER DEFAULT 0,"
    	"is_archived INTEGER DEFAULT 0,"
	"tags_csv TEXT,"
	"references_key_val_csv TEXT,"
        "created_on INTEGER,"
        "modified_on INTEGER"
        ");";


	const char* table_attachment = 
	"CREATE TABLE IF NOT EXISTS attachment("
    	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
    	"page_id INTEGER NOT NULL,"
    	"name TEXT,"
    	"mime_type TEXT,"
    	"data BLOB NOT NULL,"
    	"created_on INTEGER,"
    	"FOREIGN KEY(page_id) REFERENCES page(id)"
	");";

    rc = sqlite3_exec(db, table_metadata, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        create_popup("Error", sqlite3_errmsg(db), user_data);
        sqlite3_close(db);
        return -1;
    }

    rc = sqlite3_exec(db, table_page, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        create_popup("Error", sqlite3_errmsg(db), user_data);
        sqlite3_close(db);
        return -1;
    }

    rc = sqlite3_exec(db, table_attachment, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        create_popup("Error", sqlite3_errmsg(db), user_data);
        sqlite3_close(db);
        return -1;
    }



	const char *table_page_fts =
	    "CREATE VIRTUAL TABLE IF NOT EXISTS page_fts USING fts5("
	    "title, content='page', content_rowid='id', tokenize='trigram'"
	    ");";

	const char *triggers =
	    "CREATE TRIGGER IF NOT EXISTS page_ai AFTER INSERT ON page BEGIN "
	    "  INSERT INTO page_fts(rowid, title) VALUES (new.id, new.title); "
	    "END;"
	    "CREATE TRIGGER IF NOT EXISTS page_ad AFTER DELETE ON page BEGIN "
	    "  INSERT INTO page_fts(page_fts, rowid, title) VALUES('delete', old.id, old.title); "
	    "END;"
	    "CREATE TRIGGER IF NOT EXISTS page_au AFTER UPDATE ON page "
	    "WHEN old.title IS NOT new.title BEGIN "
	    "  INSERT INTO page_fts(page_fts, rowid, title) VALUES('delete', old.id, old.title); "
	    "  INSERT INTO page_fts(rowid, title) VALUES (new.id, new.title); "
	    "END;";

	rc = sqlite3_exec(db, table_page_fts, NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
	    create_popup("Error", sqlite3_errmsg(db), user_data);
	    sqlite3_close(db);
	    return -1;
	}

	rc = sqlite3_exec(db, triggers, NULL, NULL, NULL);
	if (rc != SQLITE_OK) {
	    create_popup("Error", sqlite3_errmsg(db), user_data);
	    sqlite3_close(db);
	    return -1;
	}













    if (ctx->create_file) {
        const char *sql =
            "INSERT INTO metadata (id, author, modified_on, created_on, comment) "
            "VALUES (?, ?, ?, ?, NULL);";

        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, sql, -1, &stmt, 0);

        sqlite3_bind_int(stmt, 1, 1);
        sqlite3_bind_text(stmt, 2, author, -1, SQLITE_TRANSIENT);

        time_t t = time(NULL);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)t);
        sqlite3_bind_int64(stmt, 4, (sqlite3_int64)t);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        ctx->total_pages = 0;
    } else {
	const char *sql =
            "UPDATE metadata SET modified_on = ?1 WHERE id = 1";

        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, sql, -1, &stmt, 0);

        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)time(NULL));

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM page", -1, &stmt, 0);

        ctx->total_pages = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            ctx->total_pages = sqlite3_column_int(stmt, 0);
	    printf("Total pages: %d\n",ctx->total_pages);
        }

        sqlite3_finalize(stmt);

        sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM page_fts", -1, &stmt, 0);

        int fts_count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            fts_count = sqlite3_column_int(stmt, 0);
        }

        sqlite3_finalize(stmt);

        if (fts_count == 0 && ctx->total_pages > 0) {
            sqlite3_exec(db,
                "INSERT INTO page_fts(rowid, title) SELECT id, title FROM page;",
                NULL, NULL, NULL);
        }
    }    

    ctx->db = db;
    out->ctx = ctx;

    return 0;
}

void notes_update_cache(Notebook *nb,int start_idx,int end_idx)
{
	Ctx* ctx = nb->ctx;
	if (start_idx > end_idx) {
	    int temp = start_idx;
	    start_idx = end_idx;
	    end_idx = temp;
	}
	if (start_idx < 0)
	    start_idx = 0;

	/* CHANGED: use search total when searching */
	int total = ctx->is_searching ? ctx->search_total : ctx->total_pages;
	if (end_idx > total)
	    end_idx = total;
	ctx->offset = start_idx;
	puts("cache rebuild");

	char buffer[1<<10];
	sqlite3_stmt *stmt;
	void (*create_popup)(const char *, const char *, void *) =
	    default_error_callback;
	void *user_data = NULL;
	(void)user_data;
	if (nb->create_popup) {
		create_popup = nb->create_popup;
		user_data = nb->user_data;
	}

	if (ctx->is_searching) {
		if (ctx->use_like_fallback) {
			snprintf(
			    buffer, sizeof(buffer),
			    "SELECT id, title FROM page WHERE title LIKE ? COLLATE "
			    "NOCASE "
			    "ORDER BY modified_on DESC, id DESC LIMIT %d OFFSET %d;",
			    end_idx - start_idx + 1, start_idx);
		} else {
			snprintf(
			    buffer, sizeof(buffer),
			    "SELECT page.id, page.title FROM page_fts "
			    "JOIN page ON page.id = page_fts.rowid "
			    "WHERE page_fts MATCH ? ORDER BY rank LIMIT %d OFFSET %d;",
			    end_idx - start_idx + 1, start_idx);
		}
	} else {
		snprintf(buffer, sizeof(buffer),
			"SELECT id, title FROM page ORDER BY modified_on DESC, id DESC LIMIT %d OFFSET %d;",
			end_idx - start_idx + 1, start_idx);
	}

	if (sqlite3_prepare_v2(ctx->db, buffer, -1, &stmt, NULL) != SQLITE_OK){
		create_popup("Error", "Interal Error Occur", NULL);
		return;
	}

	/* CHANGED: bind search query param if searching */
	if (ctx->is_searching) {
		sqlite3_bind_text(stmt, 1, ctx->search_query, -1, SQLITE_TRANSIENT);
	}

	int i = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int id = sqlite3_column_int(stmt, 0);
		const char *title = (const char *)sqlite3_column_text(stmt, 1);
		int n = sqlite3_column_bytes(stmt, 1);
		if (title) {
			if (n >= sizeof(ctx->pages[i].title))
				n = sizeof(ctx->pages[i].title) - 1;
			memcpy(ctx->pages[i].title, title, n);
			ctx->pages[i].title[n] = '\0';
		} else {
			ctx->pages[i].title[0] = '\0';
		}
		ctx->pages[i].id = id;
		i++;
	}
	sqlite3_finalize(stmt); 
}

void notes_set_search_query(Notebook *nb, const char *query)
{
	Ctx *ctx = nb->ctx;

	if (!query || query[0] == '\0') {
		ctx->is_searching = false;
		ctx->offset = -1;
		return;
	}

	int len = strlen(query);
	ctx->is_searching = true;
	ctx->use_like_fallback = (len < 3);

	if (ctx->use_like_fallback) {
		snprintf(ctx->search_query, sizeof(ctx->search_query), "%%%s%%", query); /* for LIKE */
	} else {
		snprintf(ctx->search_query, sizeof(ctx->search_query), "\"%s\"", query); /* for FTS MATCH */
	}

	sqlite3_stmt *stmt;
	const char *count_sql = ctx->use_like_fallback
		? "SELECT COUNT(*) FROM page WHERE title LIKE ? COLLATE NOCASE;"
		: "SELECT COUNT(*) FROM page_fts WHERE page_fts MATCH ?;";

	ctx->search_total = 0;
	if (sqlite3_prepare_v2(ctx->db, count_sql, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, ctx->search_query, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			ctx->search_total = sqlite3_column_int(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}

	ctx->offset = -1;
}

void notes_update_visible_view(Notebook* nb, int start_idx, int end_idx)
{
	Ctx* ctx = nb->ctx;
	if (start_idx > end_idx) {
	    int temp = start_idx;
	    start_idx = end_idx;
	    end_idx = temp;
	}
	if (start_idx < 0)
	    start_idx = 0;

	/* CHANGED: use search total when searching */
	int total = ctx->is_searching ? ctx->search_total : ctx->total_pages;
	if (end_idx > total)
	    end_idx = total;

	if (ctx->offset >= 0 
	    && start_idx >= ctx->offset 
	    && end_idx <= ctx->offset + MAX_CACHE_SIZE - 1)
	    return;
	if (start_idx + MAX_CACHE_SIZE - 1 > end_idx)
	    end_idx = start_idx + MAX_CACHE_SIZE - 1;
	if (end_idx >= start_idx + MAX_CACHE_SIZE)
	    return;
	notes_update_cache(nb,start_idx,end_idx);

}


const char* notes_get_presentation(Notebook *nb, int i)
{
	Ctx* ctx = nb->ctx;
	int total = ctx->is_searching ? ctx->search_total : ctx->total_pages;
	if (i >= total || ctx->offset < 0 || i < ctx->offset ||
	    i >= ctx->offset + MAX_CACHE_SIZE - 1)
		return NULL;
	return ctx->pages[i - ctx->offset].title;
}

Page *notes_get(Notebook *nb, int i)
{
	Ctx *ctx = nb->ctx;
	sqlite3_stmt *stmt = NULL;
	Page *p = NULL;
	char *content = NULL;

	if (i > ctx->total_pages || ctx->offset < 0 || i < ctx->offset ||
	    i >= ctx->offset + MAX_CACHE_SIZE - 1){
		return NULL;
	}
	
	if (sqlite3_prepare_v2(ctx->db, "SELECT content FROM page WHERE id = ?;", -1,
			       &stmt, NULL) != SQLITE_OK){
		
		goto fail;
	}

	p = &ctx->pages[i - ctx->offset];
	sqlite3_bind_int(stmt, 1, p->id);

	if (sqlite3_step(stmt) != SQLITE_ROW)
		goto fail;

	p->content_len = sqlite3_column_bytes(stmt, 0);

	content = calloc(p->content_len + 1, sizeof(*content));
	if (!content)
		goto fail;

	memcpy(content, sqlite3_column_text(stmt, 0), p->content_len);
	p->content = content;
	/* printf("%d\n",p->content_len); */
	sqlite3_finalize(stmt);
	return p;

fail:
	if (stmt)
		sqlite3_finalize(stmt);
	free(content);
	return NULL;
}

int notes_add(Notebook *nb, Page *page)
{

	sqlite3_stmt *stmt;
	const char *sql =
	    "INSERT INTO page (title, content, created_on, modified_on) "
	    "VALUES (?, ?, ?, ?);";
	Ctx* ctx = nb->ctx;
	sqlite3_prepare_v2(ctx->db,sql, -1, &stmt, NULL);

	sqlite3_bind_text(stmt, 1,page->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2,page->content, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL));
	sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));


	void (*create_popup)(const char *, const char *, void *) =
	    default_error_callback;
	void *user_data = NULL;
	if (nb->create_popup) {
		create_popup = nb->create_popup;
		user_data = nb->user_data;
	}

	if (sqlite3_step(stmt) != SQLITE_DONE) {
    		create_popup("Unable to save",sqlite3_errmsg(ctx->db),user_data);
		return -1;
	}

	sqlite3_finalize(stmt);
	ctx->total_pages++;

	puts("Reached");
	notes_update_cache(nb,0,MAX_CACHE_SIZE);
	return 0;
}



int notes_save(Notebook *nb, Page *page)
{
	sqlite3_stmt *stmt;
	const char *sql =
	    "UPDATE page "
	    "SET title = ?, content = ?, modified_on = ? "
	    "WHERE id = ?;";

	Ctx *ctx = nb->ctx;

	sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);

	sqlite3_bind_text(stmt, 1, page->title, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 2, page->content, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL));
	sqlite3_bind_int64(stmt, 4, (sqlite3_int64)page->id);

	void (*create_popup)(const char *, const char *, void *) =
	    default_error_callback;
	void *user_data = NULL;

	if (nb->create_popup) {
		create_popup = nb->create_popup;
		user_data = nb->user_data;
	}

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		create_popup("Unable to save",
		             sqlite3_errmsg(ctx->db),
		             user_data);
		sqlite3_finalize(stmt);
		return -1;
	}

	sqlite3_finalize(stmt);
	notes_update_cache(nb,0,MAX_CACHE_SIZE-1);
	return 0;
}


int notes_remove(Notebook *nb, int i)
{
	Page* page = notes_get(nb,i);
	void (*create_popup)(const char *, const char *, void *) =
	    default_error_callback;
	void *user_data = NULL;
	if(!page){
		create_popup("Error","Unable to delete",user_data);
		return -1;
	}
	sqlite3_stmt *stmt;

	const char *sql =
	    "DELETE FROM page WHERE id = ?;";

	Ctx *ctx = nb->ctx;

	sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);

	sqlite3_bind_int64(stmt, 1, (sqlite3_int64)page->id);


	if (nb->create_popup) {
		create_popup = nb->create_popup;
		user_data = nb->user_data;
	}

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		create_popup("Unable to delete",
		             sqlite3_errmsg(ctx->db),
		             user_data);
		sqlite3_finalize(stmt);
		return -1;
	}

	sqlite3_finalize(stmt);

	if (ctx->total_pages > 0)
		ctx->total_pages--;
	notes_update_cache(nb,0,MAX_CACHE_SIZE);	
	return 0;
}


void notes_free(Notebook *nb)
{
	Ctx* ctx = nb->ctx;
	if(!ctx)
		return;
	if(ctx->db)
		sqlite3_close_v2(ctx->db);
	free(ctx);
}

void notes_free_descriptor(BackendDescriptor *desc){
	return;
}
