///////////////////////////////////////////////////////////////////////////////
// Plus42 -- an enhanced HP-42S calculator simulator
// Copyright (C) 2004-2022  Thomas Okken
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see http://www.gnu.org/licenses/.
///////////////////////////////////////////////////////////////////////////////

#include <gtk/gtk.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>

#include <string>
#include <vector>
#include <set>

#include "shell_skin.h"
#include "shell_main.h"
#include "shell_loadimage.h"
#include "core_main.h"

using std::string;
using std::vector;
using std::set;


/**************************/
/* Skin description stuff */
/**************************/

struct SkinPoint {
    int x, y;
};

struct SkinRect {
    int x, y, width, height;
};

struct SkinKey {
    int code, shifted_code;
    SkinRect sens_rect;
    SkinRect disp_rect;
    SkinPoint src;
};

#define SKIN_MAX_MACRO_LENGTH 63

struct SkinMacro {
    int code;
    bool isName;
    unsigned char macro[SKIN_MAX_MACRO_LENGTH + 1];
    SkinMacro *next;
};

struct SkinAnnunciator {
    SkinRect disp_rect;
    SkinPoint src;
};

struct AltBackground {
    SkinRect src_rect;
    SkinPoint dst;
    int mode;
    AltBackground *next;
};

struct AltKey {
    SkinPoint src;
    int code;
    int mode;
    AltKey *next;
};

static AltBackground *alt_bak = NULL;
static AltKey *alt_key = NULL;

static SkinRect skin;
static SkinPoint display_loc;
static double display_scale_x;
static double display_scale_y;
static bool display_scale_int;
static SkinColor display_bg, display_fg;
static SkinKey *keylist = NULL;
static int nkeys = 0;
static int keys_cap = 0;
static SkinMacro *macrolist = NULL;
static SkinAnnunciator annunciators[7];
static int disp_r, disp_c, disp_w, disp_h;

static FILE *external_file;
static long builtin_length;
static long builtin_pos;
static const unsigned char *builtin_file;

static GdkPixbuf *skin_image = NULL;
static int skin_y;
static int skin_type;
static const SkinColor *skin_cmap;

static char *disp_bits = NULL;
static int disp_bpl;

static vector<string> skin_labels;

static keymap_entry *keymap = NULL;
static int keymap_length;

static bool display_enabled = true;
static int skin_mode = 0;


/**********************************************************/
/* Linked-in skins; defined in the skins.c, which in turn */
/* is generated by skin2c.c under control of skin2c.conf  */
/**********************************************************/

extern const int skin_count;
extern const char * const skin_name[];
extern const long skin_layout_size[];
extern const unsigned char * const skin_layout_data[];
extern const long skin_bitmap_size[];
extern const unsigned char * const skin_bitmap_data[];


/*******************/
/* Local functions */
/*******************/

static void addMenuItem(GtkMenu *menu, const char *name, bool enabled);
static void selectSkinCB(GtkWidget *w, gpointer cd);
static bool skin_open(const char *name, bool open_layout, bool force_builtin);
static int skin_gets(char *buf, int buflen);
static void skin_close();


static void addMenuItem(GtkMenu *menu, const char *name, bool enabled) {
    bool checked = false;
    if (enabled) {
        if (state.skinName[0] == 0) {
            strcpy(state.skinName, name);
            checked = true;
        } else if (strcmp(state.skinName, name) == 0)
            checked = true;
    }

    GtkWidget *w = gtk_check_menu_item_new_with_label(name);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w), checked);
    gtk_widget_set_sensitive(w, enabled);

    // Apparently, there is no way to retrieve the label from a menu item,
    // so I have to store them and pass them to the callback explicitly.
    skin_labels.push_back(name);
    const char *lbl = skin_labels.back().c_str();
    g_signal_connect(G_OBJECT(w), "activate",
                     G_CALLBACK(selectSkinCB), (gpointer) lbl);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), w);
    gtk_widget_show(w);
}

static void selectSkinCB(GtkWidget *w, gpointer cd) {
    char *name = (char *) cd;
    strcpy(state.skinName, name);
    update_skin(-1, -1);
}

void update_skin(int rows, int cols) {
    int w, h, flags;
    skin_load(&w, &h, &rows, &cols, &flags);
    disp_rows = rows;
    disp_cols = cols;
    core_repaint_display(disp_rows, disp_cols, flags);
    gtk_widget_set_size_request(calc_widget, w, h);
    gtk_widget_queue_draw(calc_widget);
}

void shell_set_skin_mode(int mode) {
    int old_mode = skin_mode;
    skin_mode = mode;
    if (skin_mode != old_mode && calc_widget != NULL)
        gtk_widget_queue_draw(calc_widget);
}

static bool skin_open(const char *name, bool open_layout, bool force_builtin) {
    if (!force_builtin) {
        const char *suffix = open_layout ? ".layout" : ".gif";
        // Try Plus42 dir first...
        string fname = string(free42dirname) + "/" + name + suffix;
        external_file = fopen(fname.c_str(), "r");
        if (external_file != NULL)
            return true;
        // Next, shared dirs...
        const char *xdg_data_dirs = getenv("XDG_DATA_DIRS");
        if (xdg_data_dirs == NULL || xdg_data_dirs[0] == 0)
            xdg_data_dirs = "/usr/local/share:/usr/share";
        char *buf = (char *) malloc(strlen(xdg_data_dirs) + 1);
        strcpy(buf, xdg_data_dirs);
        char *tok = strtok(buf, ":");
        while (tok != NULL) {
            string dirname = tok;
            string fname = dirname + "/plus42/" + name + suffix;
            external_file = fopen(fname.c_str(), "r");
            if (external_file != NULL) {
                free(buf);
                return true;
            }
            fname = dirname + "/plus42/skins/" + name + suffix;
            external_file = fopen(fname.c_str(), "r");
            if (external_file != NULL) {
                free(buf);
                return true;
            }
            tok = strtok(NULL, ":");
        }
        free(buf);
    }

    // Look for built-in skin last
    for (int i = 0; i < skin_count; i++) {
        if (strcmp(name, skin_name[i]) == 0) {
            external_file = NULL;
            builtin_pos = 0;
            if (open_layout) {
                builtin_length = skin_layout_size[i];
                builtin_file = skin_layout_data[i];
            } else {
                builtin_length = skin_bitmap_size[i];
                builtin_file = skin_bitmap_data[i];
            }
            return true;
        }
    }

    // Nothing found.
    return false;
}

int skin_getchar() {
    if (external_file != NULL)
        return fgetc(external_file);
    else if (builtin_pos < builtin_length)
        return builtin_file[builtin_pos++];
    else
        return EOF;
}

static int skin_gets(char *buf, int buflen) {
    int p = 0;
    int eof = -1;
    int comment = 0;
    while (p < buflen - 1) {
        int c = skin_getchar();
        if (eof == -1)
            eof = c == EOF;
        if (c == EOF || c == '\n' || c == '\r')
            break;
        /* Remove comments */
        if (c == '#')
            comment = 1;
        if (comment)
            continue;
        /* Suppress leading spaces */
        if (p == 0 && isspace(c))
            continue;
        buf[p++] = c;
    }
    buf[p++] = 0;
    return p > 1 || !eof;
}

static void skin_close() {
    if (external_file != NULL)
        fclose(external_file);
}

static void scan_skin_dir(const char *dirname, set<string> &names) {
    DIR *dir = opendir(dirname);
    if (dir == NULL)
        return;

    struct dirent *dent;
    while ((dent = readdir(dir)) != NULL) {
        int namelen = strlen(dent->d_name);
        if (namelen < 7)
            continue;
        if (strcmp(dent->d_name + namelen - 7, ".layout") != 0)
            continue;
        string name = dent->d_name;
        name.erase(namelen - 7);
        names.insert(name);
    }
    closedir(dir);
}

void skin_menu_update(GtkWidget *w) {
    GtkMenu *skin_menu = (GtkMenu *) gtk_menu_item_get_submenu(GTK_MENU_ITEM(w));
    GList *children = gtk_container_get_children(GTK_CONTAINER(skin_menu));
    GList *item = children;
    while (item != NULL) {
        gtk_widget_destroy(GTK_WIDGET(item->data));
        item = item->next;
    }
    g_list_free(children);

    skin_labels.clear();

    set<string> shared_skins;
    const char *xdg_data_dirs = getenv("XDG_DATA_DIRS");
    if (xdg_data_dirs == NULL || xdg_data_dirs[0] == 0)
        xdg_data_dirs = "/usr/local/share:/usr/share";
    char *buf = (char *) malloc(strlen(xdg_data_dirs) + 1);
    strcpy(buf, xdg_data_dirs);
    char *tok = strtok(buf, ":");
    while (tok != NULL) {
        string dirname = tok;
        scan_skin_dir((dirname + "/plus42").c_str(), shared_skins);
        scan_skin_dir((dirname + "/plus42/skins").c_str(), shared_skins);
        tok = strtok(NULL, ":");
    }
    free(buf);

    set<string> private_skins;
    scan_skin_dir(free42dirname, private_skins);

    for (int i = 0; i < skin_count; i++) {
        const char *name = skin_name[i];
        bool enabled = private_skins.find(name) == private_skins.end()
                        && shared_skins.find(name) == shared_skins.end();
        addMenuItem(skin_menu, name, enabled);
    }

    if (!shared_skins.empty()) {
        GtkWidget *w = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(skin_menu), w);
        gtk_widget_show(w);

        for (set<string>::const_iterator i = shared_skins.begin(); i != shared_skins.end(); i++) {
            const char *name = i->c_str();
            bool enabled = private_skins.find(name) == private_skins.end();
            addMenuItem(skin_menu, name, enabled);
        }
    }

    if (!private_skins.empty()) {
        GtkWidget *w = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(skin_menu), w);
        gtk_widget_show(w);

        for (set<string>::const_iterator i = private_skins.begin(); i != private_skins.end(); i++)
            addMenuItem(skin_menu, i->c_str(), true);
    }
}

void skin_load(int *width, int *height, int *rows, int *cols, int *flags) {
    char line[1024];
    int lineno = 0;
    bool force_builtin = false;

    static int last_req_rows, last_req_cols;
    if (*rows == -1) {
        *rows = last_req_rows;
        *cols = last_req_cols;
    } else {
        last_req_rows = *rows;
        last_req_cols = *cols;
    }

    int requested_rows = *rows;
    int requested_cols = *cols;

    if (state.skinName[0] == 0) {
        fallback_on_1st_builtin_skin:
        strcpy(state.skinName, skin_name[0]);
        force_builtin = true;
    }

    /*************************/
    /* Load skin description */
    /*************************/

    if (!skin_open(state.skinName, 1, force_builtin))
        goto fallback_on_1st_builtin_skin;

    if (keylist != NULL)
        free(keylist);
    keylist = NULL;
    nkeys = 0;
    keys_cap = 0;

    while (alt_bak != NULL) {
        AltBackground *n = alt_bak->next;
        free(alt_bak);
        alt_bak = n;
    }

    while (alt_key != NULL) {
        AltKey *n = alt_key->next;
        free(alt_key);
        alt_key = n;
    }

    while (macrolist != NULL) {
        SkinMacro *m = macrolist->next;
        free(macrolist);
        macrolist = m;
    }

    if (keymap != NULL)
        free(keymap);
    keymap = NULL;
    keymap_length = 0;
    int kmcap = 0;

    int disp_rows = 2;
    int disp_cols = 22;
    int fl = 0;

    int alt_disp_y = -1;
    int alt_pixel_height = -1;
    int max_r = -1;
    int dup_first_y = 0, dup_last_y = 0;

    while (skin_gets(line, 1024)) {
        lineno++;
        if (*line == 0)
            continue;
        if (strncasecmp(line, "skin:", 5) == 0) {
            int x, y, width, height;
            if (sscanf(line + 5, " %d,%d,%d,%d", &x, &y, &width, &height) == 4){
                skin.x = x;
                skin.y = y;
                skin.width = width;
                skin.height = height;
            }
        } else if (strncasecmp(line, "display:", 8) == 0) {
            int x, y;
            double xscale, yscale;
            unsigned long bg, fg;
            if (sscanf(line + 8, " %d,%d %lf %lf %lx %lx", &x, &y,
                                            &xscale, &yscale, &bg, &fg) == 6) {
                display_loc.x = x;
                display_loc.y = y;
                display_scale_x = xscale;
                display_scale_y = yscale;
                display_bg.r = (unsigned char) (bg >> 16);
                display_bg.g = (unsigned char) (bg >> 8);
                display_bg.b = (unsigned char) bg;
                display_fg.r = (unsigned char) (fg >> 16);
                display_fg.g = (unsigned char) (fg >> 8);
                display_fg.b = (unsigned char) fg;
            }
        } else if (strncasecmp(line, "displaysize:", 12) == 0) {
            int c, r, n = -1, p = -1, m = -1;
            if (sscanf(line + 12, " %d,%d %d %d %d", &c, &r, &n, &p, &m) >= 2) {
                if (c >= 22 && r >= 2) {
                    disp_rows = r;
                    disp_cols = c;
                    if (n != -1)
                        alt_disp_y = n;
                    if (p != -1)
                        alt_pixel_height = p;
                    if (m != -1)
                        max_r = m;
                }
            }
        } else if (strncasecmp(line, "displayexpansionzone:", 21) == 0) {
            int first, last;
            if (sscanf(line + 21, " %d %d", &first, &last) == 2) {
                dup_first_y = first;
                dup_last_y = last;
            }
        } else if (strncasecmp(line, "key:", 4) == 0) {
            char keynumbuf[20];
            int keynum, shifted_keynum;
            int sens_x, sens_y, sens_width, sens_height;
            int disp_x, disp_y, disp_width, disp_height;
            int act_x, act_y;
            if (sscanf(line + 4, " %s %d,%d,%d,%d %d,%d,%d,%d %d,%d",
                                 keynumbuf,
                                 &sens_x, &sens_y, &sens_width, &sens_height,
                                 &disp_x, &disp_y, &disp_width, &disp_height,
                                 &act_x, &act_y) == 11) {
                int n = sscanf(keynumbuf, "%d,%d", &keynum, &shifted_keynum);
                if (n > 0) {
                    if (n == 1)
                        shifted_keynum = keynum;
                    SkinKey *key;
                    if (nkeys == keys_cap) {
                        keys_cap += 50;
                        keylist = (SkinKey *)
                                realloc(keylist, keys_cap * sizeof(SkinKey));
                        // TODO - handle memory allocation failure
                    }
                    key = keylist + nkeys;
                    key->code = keynum;
                    key->shifted_code = shifted_keynum;
                    key->sens_rect.x = sens_x;
                    key->sens_rect.y = sens_y;
                    key->sens_rect.width = sens_width;
                    key->sens_rect.height = sens_height;
                    key->disp_rect.x = disp_x;
                    key->disp_rect.y = disp_y;
                    key->disp_rect.width = disp_width;
                    key->disp_rect.height = disp_height;
                    key->src.x = act_x;
                    key->src.y = act_y;
                    nkeys++;
                }
            }
        } else if (strncasecmp(line, "macro:", 6) == 0) {
            char *quot1 = strchr(line + 6, '"');
            if (quot1 != NULL) {
                char *quot2 = strrchr(line + 6, '"');
                if (quot2 != quot1) {
                    long len = quot2 - quot1 - 1;
                    if (len > SKIN_MAX_MACRO_LENGTH)
                        len = SKIN_MAX_MACRO_LENGTH;
                    int n;
                    if (sscanf(line + 6, "%d", &n) == 1 && n >= 38 && n <= 255) {
                        SkinMacro *macro = (SkinMacro *) malloc(sizeof(SkinMacro));
                        // TODO - handle memory allocation failure
                        macro->code = n;
                        macro->isName = true;
                        memcpy(macro->macro, quot1 + 1, len);
                        macro->macro[len] = 0;
                        macro->next = macrolist;
                        macrolist = macro;
                    }
                }
            } else {
                char *tok = strtok(line + 6, " \t");
                int len = 0;
                SkinMacro *macro = NULL;
                while (tok != NULL) {
                    char *endptr;
                    long n = strtol(tok, &endptr, 10);
                    if (*endptr != 0) {
                        /* Not a proper number; ignore this macro */
                        if (macro != NULL) {
                            free(macro);
                            macro = NULL;
                            break;
                        }
                    }
                    if (macro == NULL) {
                        if (n < 38 || n > 255)
                            /* Macro code out of range; ignore this macro */
                            break;
                        macro = (SkinMacro *) malloc(sizeof(SkinMacro));
                        // TODO - handle memory allocation failure
                        macro->code = n;
                        macro->isName = false;
                    } else if (len < SKIN_MAX_MACRO_LENGTH) {
                        if (n < 1 || n > 37) {
                            /* Key code out of range; ignore this macro */
                            free(macro);
                            macro = NULL;
                            break;
                        }
                        macro->macro[len++] = n;
                    }
                    tok = strtok(NULL, " \t");
                }
                if (macro != NULL) {
                    macro->macro[len++] = 0;
                    macro->next = macrolist;
                    macrolist = macro;
                }
            }
        } else if (strncasecmp(line, "annunciator:", 12) == 0) {
            int annnum;
            int disp_x, disp_y, disp_width, disp_height;
            int act_x, act_y;
            if (sscanf(line + 12, " %d %d,%d,%d,%d %d,%d",
                                  &annnum,
                                  &disp_x, &disp_y, &disp_width, &disp_height,
                                  &act_x, &act_y) == 7) {
                if (annnum >= 1 && annnum <= 7) {
                    SkinAnnunciator *ann = annunciators + (annnum - 1);
                    ann->disp_rect.x = disp_x;
                    ann->disp_rect.y = disp_y;
                    ann->disp_rect.width = disp_width;
                    ann->disp_rect.height = disp_height;
                    ann->src.x = act_x;
                    ann->src.y = act_y;
                }
            }
        } else if (strncasecmp(line, "gtkkey:", 7) == 0) {
            keymap_entry *entry = parse_keymap_entry(line + 7, lineno);
            if (entry != NULL) {
                if (keymap_length == kmcap) {
                    kmcap += 50;
                    keymap = (keymap_entry *)
                                realloc(keymap, kmcap * sizeof(keymap_entry));
                    // TODO - handle memory allocation failure
                }
                memcpy(keymap + (keymap_length++), entry, sizeof(keymap_entry));
            }
        } else if (strncasecmp(line, "flags:", 6) == 0) {
            sscanf(line + 6, "%d", &fl);
        } else if (strncasecmp(line, "altbkgd:", 8) == 0) {
            int mode;
            int src_x, src_y, src_width, src_height;
            int dst_x, dst_y;
            if (sscanf(line + 8, " %d %d,%d,%d,%d %d,%d",
                       &mode,
                       &src_x, &src_y, &src_width, &src_height,
                       &dst_x, &dst_y) == 7) {
                AltBackground *ab = (AltBackground *) malloc(sizeof(AltBackground));
                ab->src_rect.x = src_x;
                ab->src_rect.y = src_y;
                ab->src_rect.width = src_width;
                ab->src_rect.height = src_height;
                ab->dst.x = dst_x;
                ab->dst.y = dst_y;
                ab->mode = mode;
                ab->next = NULL;
                if (alt_bak == NULL) {
                    alt_bak = ab;
                } else {
                    AltBackground *p = alt_bak;
                    while (p->next != NULL)
                        p = p->next;
                    p->next = ab;
                }
            }
        } else if (strncasecmp(line, "altkey:", 7) == 0) {
            int mode;
            int code;
            int src_x, src_y;
            if (sscanf(line + 8, " %d %d %d,%d",
                       &mode, &code, &src_x, &src_y) == 4) {
                AltKey *ak = (AltKey *) malloc(sizeof(AltKey));
                ak->src.x = src_x;
                ak->src.y = src_y;
                ak->mode = mode;
                ak->code = code;
                ak->next = NULL;
                if (alt_key == NULL) {
                    alt_key = ak;
                } else {
                    AltKey *p = alt_key;
                    while (p->next != NULL)
                        p = p->next;
                    p->next = ak;
                }
            }
        }
    }

    int max_h = -1;
    if (max_r != -1) {
        double r = max_r == 2
                    ? display_scale_y
                    : alt_pixel_height != -1
                        ? alt_pixel_height
                        : display_scale_x;
        max_h = (int) (max_r * r);
        double r2 = (alt_pixel_height != -1 ? (double) alt_pixel_height : display_scale_x)
                        * disp_cols / requested_cols;
        if (requested_rows * r2 > max_h)
            requested_rows = (int) (max_h / r2);
    }

    double xs = display_scale_x;
    double ys = requested_rows == 2
                    ? display_scale_y
                    : alt_pixel_height != -1
                        ? alt_pixel_height
                        : display_scale_x;
    int available = (int) ((disp_rows == 2
                        ? display_scale_y
                        : alt_pixel_height != -1
                            ? alt_pixel_height
                            : display_scale_x)
                    * disp_rows * 8);

    xs = xs * disp_cols / requested_cols;
    ys = ys * disp_cols / requested_cols;

    /* Calculate how many extra pixels we need. And force pixels to be square */
    int extra = (int) (requested_rows * ys * 8 - available);
    int wasted = 0;
    if (extra > 0) {
        if (dup_first_y == 0 && dup_last_y == 0) {
            dup_first_y = display_loc.y;
            dup_last_y = (int) (display_loc.y + display_scale_y * 16);
        }
        /* Fix coordinates */
        skin.height += extra;
        for (int i = 0; i < 7; i++) {
            SkinAnnunciator *ann = annunciators + i;
            if (ann->disp_rect.y > dup_first_y)
                ann->disp_rect.y += extra;
            if (ann->src.y > dup_first_y)
                ann->src.y += extra;
        }
        for (int i = 0; i < nkeys; i++) {
            SkinKey *key = keylist + i;
            if (key->sens_rect.y > dup_first_y)
                key->sens_rect.y += extra;
            if (key->disp_rect.y > dup_first_y)
                key->disp_rect.y += extra;
            if (key->src.y > dup_first_y)
                key->src.y += extra;
        }
        for (AltBackground *ab = alt_bak; ab != NULL; ab = ab->next) {
            if (ab->src_rect.y > dup_first_y)
                ab->src_rect.y += extra;
            if (ab->dst.y > dup_first_y)
                ab->dst.y += extra;
        }
        for (AltKey *ak = alt_key; ak != NULL; ak = ak->next) {
            if (ak->src.y > dup_first_y)
                ak->src.y += extra;
        }
    } else if (extra < 0) {
        wasted = -extra;
        extra = 0;
    }

    if (requested_rows > 2 && alt_disp_y != -1)
        display_loc.y = alt_disp_y + wasted;

    disp_rows = requested_rows;
    disp_cols = requested_cols;
    display_scale_x = xs;
    display_scale_y = ys;
    display_scale_int = xs == (int) xs && ys == (int) ys;

    skin_close();

    /********************/
    /* Load skin bitmap */
    /********************/

    if (!skin_open(state.skinName, 0, force_builtin))
        goto fallback_on_1st_builtin_skin;

    /* shell_loadimage() calls skin_getchar() to load the image from the
     * compiled-in or on-disk file; it calls skin_init_image(),
     * skin_put_pixels(), and skin_finish_image() to create the in-memory
     * representation.
     */
    bool success = shell_loadimage(extra, dup_first_y, dup_last_y);
    skin_close();

    if (!success)
        goto fallback_on_1st_builtin_skin;

    *width = skin.width;
    *height = skin.height;

    /********************************/
    /* (Re)build the display bitmap */
    /********************************/

    disp_r = disp_rows;
    disp_c = disp_cols;
    disp_w = disp_cols * 6 - 1;
    disp_h = disp_rows * 8;

    if (disp_bits != NULL)
        free(disp_bits);
    disp_bpl = (disp_w + 7) >> 3;
    int size = disp_bpl * disp_h;
    disp_bits = (char *) malloc(size);
    memset(disp_bits, 0, size);

    *rows = disp_rows;
    *cols = disp_cols;
    *flags = fl;
}

int skin_init_image(int type, int ncolors, const SkinColor *colors,
                    int width, int height) {
    if (skin_image != NULL) {
        g_object_unref(skin_image);
        skin_image = NULL;
    }

    skin_image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);

    skin_y = 0;
    skin_type = type;
    skin_cmap = colors;
    return 1;
}

void skin_put_pixels(unsigned const char *data) {
    guchar *pix = gdk_pixbuf_get_pixels(skin_image);
    int bytesperline = gdk_pixbuf_get_rowstride(skin_image);
    int width = gdk_pixbuf_get_width(skin_image);
    guchar *p = pix + skin_y * bytesperline;

    if (skin_type == IMGTYPE_MONO) {
        for (int x = 0; x < width; x++) {
            guchar c;
            if ((data[x >> 3] & (1 << (x & 7))) == 0)
                c = 0;
            else
                c = 255;
            *p++ = c;
            *p++ = c;
            *p++ = c;
        }
    } else if (skin_type == IMGTYPE_GRAY) {
        for (int x = 0; x < width; x++) {
            guchar c = data[x];
            *p++ = c;
            *p++ = c;
            *p++ = c;
        }
    } else if (skin_type == IMGTYPE_COLORMAPPED) {
        for (int x = 0; x < width; x++) {
            guchar c = data[x];
            *p++ = skin_cmap[c].r;
            *p++ = skin_cmap[c].g;
            *p++ = skin_cmap[c].b;
        }
    } else { // skin_type == IMGTYPE_TRUECOLOR
        int xx = 0;
        for (int x = 0; x < width; x++) {
            *p++ = data[xx++];
            *p++ = data[xx++];
            *p++ = data[xx++];
        }
    }

    skin_y++;
}

void skin_finish_image() {
    // Nothing to do.
}

void skin_repaint(cairo_t *cr) {
    gdk_cairo_set_source_pixbuf(cr, skin_image, -skin.x, -skin.y);
    cairo_paint(cr);
    if (skin_mode != 0)
        for (AltBackground *ab = alt_bak; ab != NULL; ab = ab->next)
            if (ab->mode == skin_mode) {
                cairo_save(cr);
                gdk_cairo_set_source_pixbuf(cr, skin_image,
                        ab->dst.x - ab->src_rect.x - skin.x,
                        ab->dst.y - ab->src_rect.y - skin.y);
                cairo_rectangle(cr, ab->dst.x, ab->dst.y, ab->src_rect.width, ab->src_rect.height);
                cairo_clip(cr);
                cairo_paint(cr);
                cairo_restore(cr);
            }
}

void skin_repaint_annunciator(cairo_t *cr, int which) {
    if (!display_enabled)
        return;
    SkinAnnunciator *ann = annunciators + (which - 1);
    cairo_save(cr);
    gdk_cairo_set_source_pixbuf(cr, skin_image,
            ann->disp_rect.x - ann->src.x - skin.x,
            ann->disp_rect.y - ann->src.y - skin.y);
    cairo_rectangle(cr, ann->disp_rect.x, ann->disp_rect.y, ann->disp_rect.width, ann->disp_rect.height);
    cairo_clip(cr);
    cairo_paint(cr);
    cairo_restore(cr);
}

void skin_invalidate_annunciator(GdkWindow *win, int which) {
    if (!display_enabled)
        return;
    SkinAnnunciator *ann = annunciators + (which - 1);
    GdkRectangle clip;
    clip.x = ann->disp_rect.x;
    clip.y = ann->disp_rect.y;
    clip.width = ann->disp_rect.width;
    clip.height = ann->disp_rect.height;
    gdk_window_invalidate_rect(win, &clip, FALSE);
}

void skin_find_key(int x, int y, bool cshift, int *skey, int *ckey) {
    int i;
    if (core_menu()
            && x >= display_loc.x
            && x < display_loc.x + disp_w * display_scale_x
            && y >= display_loc.y + (disp_h - 7) * display_scale_y
            && y < display_loc.y + disp_h * display_scale_y) {
        int softkey = (x - display_loc.x) / (disp_c * display_scale_x) + 1;
        *skey = -1 - softkey;
        *ckey = softkey;
        return;
    }
    for (i = 0; i < nkeys; i++) {
        SkinKey *k = keylist + i;
        int rx = x - k->sens_rect.x;
        int ry = y - k->sens_rect.y;
        if (rx >= 0 && rx < k->sens_rect.width
                && ry >= 0 && ry < k->sens_rect.height) {
            *skey = i;
            *ckey = cshift ? k->shifted_code : k->code;
            return;
        }
    }
    *skey = -1;
    *ckey = 0;
}

int skin_find_skey(int ckey) {
    int i;
    for (i = 0; i < nkeys; i++)
        if (keylist[i].code == ckey || keylist[i].shifted_code == ckey)
            return i;
    return -1;
}

unsigned char *skin_find_macro(int ckey, bool *is_name) {
    SkinMacro *m = macrolist;
    while (m != NULL) {
        if (m->code == ckey) {
            *is_name = m->isName;
            return m->macro;
        }
        m = m->next;
    }
    return NULL;
}

unsigned char *skin_keymap_lookup(guint keyval, bool printable,
                                  bool ctrl, bool alt, bool shift, bool cshift,
                                  bool *exact) {
    int i;
    unsigned char *macro = NULL;
    for (i = 0; i < keymap_length; i++) {
        keymap_entry *entry = keymap + i;
        if (ctrl == entry->ctrl
                && alt == entry->alt
                && (printable || shift == entry->shift)
                && keyval == entry->keyval) {
            if (cshift == entry->cshift) {
                *exact = true;
                return entry->macro;
            }
            if (cshift)
                macro = entry->macro;
        }
    }
    *exact = false;
    return macro;
}

void skin_repaint_key(cairo_t *cr, int key, bool state) {
    SkinKey *k;

    if (key >= -7 && key <= -2) {
        /* Soft key */
        if (!display_enabled)
            // Should never happen -- the display is only disabled during macro
            // execution, and softkey events should be impossible to generate
            // in that state. But, just staying on the safe side.
            return;
        key = -1 - key;
        int x = (key - 1) * disp_c;
        int y = disp_h - 7;
        int width = (disp_c - 1);
        int height = 7;

        cairo_save(cr);
        cairo_translate(cr, display_loc.x, display_loc.y);
        cairo_scale(cr, display_scale_x, display_scale_y);
        cairo_rectangle(cr, x, y, width, height);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, display_bg.r / 255.0, display_bg.g / 255.0, display_bg.b / 255.0);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, display_fg.r / 255.0, display_fg.g / 255.0, display_fg.b / 255.0);

        for (int v = y; v < y + height; v++)
            for (int h = x; h < x + width; h++)
                if (((disp_bits[v * 17 + (h >> 3)] & (1 << (h & 7))) != 0) != state) {
                    cairo_rectangle(cr, h, v, 1, 1);
                    cairo_fill(cr);
                }

        cairo_restore(cr);
        return;
    }

    if (key < 0 || key >= nkeys)
        return;
    k = keylist + key;

    cairo_save(cr);
    cairo_rectangle(cr, k->disp_rect.x, k->disp_rect.y, k->disp_rect.width, k->disp_rect.height);
    cairo_clip(cr);

    if (state) {
        int sx = k->src.x, sy = k->src.y;
        if (skin_mode != 0)
            for (AltKey *ak = alt_key; ak != NULL; ak = ak->next)
                if (ak->mode == skin_mode && ak->code == k->code) {
                    sx = ak->src.x;
                    sy = ak->src.y;
                    break;
                }
        gdk_cairo_set_source_pixbuf(cr, skin_image,
                k->disp_rect.x - sx - skin.x,
                k->disp_rect.y - sy - skin.y);
        cairo_paint(cr);
    } else {
        gdk_cairo_set_source_pixbuf(cr, skin_image, -skin.x, -skin.y);
        cairo_paint(cr);
        if (skin_mode != 0)
            for (AltBackground *ab = alt_bak; ab != NULL; ab = ab->next)
                if (ab->mode == skin_mode
                        && k->disp_rect.x >= ab->dst.x && k->disp_rect.x < ab->dst.x + ab->src_rect.width
                        && k->disp_rect.y >= ab->dst.y && k->disp_rect.y < ab->dst.y + ab->src_rect.height) {
                    gdk_cairo_set_source_pixbuf(cr, skin_image,
                            k->disp_rect.x - ab->dst.x + ab->src_rect.x - skin.x,
                            k->disp_rect.y - ab->dst.y + ab->src_rect.y - skin.y);
                    cairo_paint(cr);
                }
    }

    cairo_restore(cr);
}

void skin_invalidate_key(GdkWindow *win, int key) {
    if (!display_enabled)
        return;
    if (key >= -7 && key <= -2) {
        /* Soft key */
        key = -1 - key;
        int x = (key - 1) * disp_c * display_scale_x;
        int y = (disp_h - 7) * display_scale_y;
        int width = (disp_c - 1) * display_scale_x;
        int height = 7 * display_scale_y;
        GdkRectangle clip;
        clip.x = display_loc.x + x;
        clip.y = display_loc.y + y;
        clip.width = width;
        clip.height = height;
        gdk_window_invalidate_rect(win, &clip, FALSE);
        return;
    }
    if (key < 0 || key >= nkeys)
        return;
    SkinKey *k = keylist + key;
    GdkRectangle clip;
    clip.x = k->disp_rect.x;
    clip.y = k->disp_rect.y;
    clip.width = k->disp_rect.width;
    clip.height = k->disp_rect.height;
    gdk_window_invalidate_rect(win, &clip, FALSE);
}

void skin_display_invalidater(GdkWindow *win, const char *bits, int bytesperline,
                                        int x, int y, int width, int height) {
    for (int v = y; v < y + height; v++)
        for (int h = x; h < x + width; h++)
            if ((bits[v * bytesperline + (h >> 3)] & (1 << (h & 7))) != 0)
                disp_bits[v * disp_bpl + (h >> 3)] |= 1 << (h & 7);
            else
                disp_bits[v * disp_bpl + (h >> 3)] &= ~(1 << (h & 7));

    if (win != NULL) {
        if (allow_paint && display_enabled) {
            GdkRectangle clip;
            clip.x = display_loc.x + x * display_scale_x;
            clip.y = display_loc.y + y * display_scale_y;
            clip.width = width * display_scale_x;
            clip.height = height * display_scale_y;
            gdk_window_invalidate_rect(win, &clip, FALSE);
        }
    } else {
        gtk_widget_queue_draw_area(calc_widget,
                display_loc.x - display_scale_x,
                display_loc.y - display_scale_y,
                (disp_w + 2) * display_scale_x,
                (disp_h + 2) * display_scale_y);
    }
}

void skin_repaint_display(cairo_t *cr) {
    if (!display_enabled)
        return;
    cairo_save(cr);
    cairo_translate(cr, display_loc.x, display_loc.y);
    cairo_scale(cr, display_scale_x, display_scale_y);
    cairo_rectangle(cr, -1, -1, disp_w + 2, disp_h + 2);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, display_bg.r / 255.0, display_bg.g / 255.0, display_bg.b / 255.0);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, display_fg.r / 255.0, display_fg.g / 255.0, display_fg.b / 255.0);
    for (int v = 0; v < disp_h; v++) {
        for (int h = 0; h < disp_w; h++) {
            if ((disp_bits[v * disp_bpl + (h >> 3)] & (1 << (h & 7))) != 0) {
                cairo_rectangle(cr, h, v, 1, 1);
                cairo_fill(cr);
            }
        }
    }
    cairo_restore(cr);
}

void skin_invalidate_display(GdkWindow *win) {
    if (display_enabled) {
        GdkRectangle clip;
        clip.x = display_loc.x;
        clip.y = display_loc.y;
        clip.width = disp_w * display_scale_x;
        clip.height = disp_h * display_scale_y;
        gdk_window_invalidate_rect(win, &clip, FALSE);
    }
}

void skin_display_set_enabled(bool enable) {
    display_enabled = enable;
}