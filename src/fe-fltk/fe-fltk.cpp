// FLTK-based frontend implementing a functional UI (tabs, text, userlist).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <chrono>
#include <cctype>
#include <locale.h>
#include <map>
#include <string>
#include <vector>
#include <list>
#include <set>
#include <dlfcn.h>

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Select_Browser.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Progress.H>
#include <FL/fl_ask.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_Preferences.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Value_Input.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Tile.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Tree.H>

extern "C" {
#include "../common/hexchat.h"
#include "../common/hexchatc.h"
#include "../common/cfgfiles.h"
#include "../common/outbound.h"
#include "../common/util.h"
#include "../common/fe.h"
#include "../common/server.h"
#include "../common/dcc.h"
#include "../common/notify.h"
#include "../common/ignore.h"
#include "../common/text.h"
#include "../common/servlist.h"
#include "../common/url.h"
#include "../common/tree.h"
}

// ============================================================================
// Enchant Spell Checking Support
// ============================================================================
// Runtime loading of enchant library for spell checking in the input box.
// This allows spell checking to be optional - if enchant is not installed,
// the application still works normally without spell checking.

struct EnchantDict;
struct EnchantBroker;

typedef void (*EnchantDictDescribeFn)(const char *const lang_tag,
                                       const char *const provider_name,
                                       const char *const provider_desc,
                                       const char *const provider_file,
                                       void *user_data);

// Function pointers loaded at runtime from libenchant
static EnchantBroker *(*enchant_broker_init)(void) = nullptr;
static void (*enchant_broker_free)(EnchantBroker *broker) = nullptr;
static void (*enchant_broker_free_dict)(EnchantBroker *broker, EnchantDict *dict) = nullptr;
static void (*enchant_broker_list_dicts)(EnchantBroker *broker, EnchantDictDescribeFn fn, void *user_data) = nullptr;
static EnchantDict *(*enchant_broker_request_dict)(EnchantBroker *broker, const char *const tag) = nullptr;
static void (*enchant_dict_add_to_personal)(EnchantDict *dict, const char *const word, ssize_t len) = nullptr;
static void (*enchant_dict_add_to_session)(EnchantDict *dict, const char *const word, ssize_t len) = nullptr;
static int (*enchant_dict_check)(EnchantDict *dict, const char *const word, ssize_t len) = nullptr;
static void (*enchant_dict_describe)(EnchantDict *dict, EnchantDictDescribeFn fn, void *user_data) = nullptr;
static void (*enchant_dict_free_suggestions)(EnchantDict *dict, char **suggestions) = nullptr;
static char **(*enchant_dict_suggest)(EnchantDict *dict, const char *const word, ssize_t len, size_t *out_n_suggs) = nullptr;

static bool have_enchant = false;
static void *enchant_handle = nullptr;
static EnchantBroker *spell_broker = nullptr;
static std::vector<EnchantDict *> spell_dicts;
static std::set<std::string> spell_session_ignores;

static void
initialize_enchant(void)
{
	if (enchant_handle)
		return;

	const char *libnames[] = {
		"libenchant-2.so.2",
		"libenchant.so.2",
		"libenchant.so.1",
		"libenchant.so",
		nullptr
	};

	for (int i = 0; libnames[i]; i++)
	{
		enchant_handle = dlopen(libnames[i], RTLD_LAZY);
		if (enchant_handle)
			break;
	}

	if (!enchant_handle)
		return;

#define LOAD_SYM(name) \
	*(void **)(&name) = dlsym(enchant_handle, #name); \
	if (!name) { dlclose(enchant_handle); enchant_handle = nullptr; return; }

	LOAD_SYM(enchant_broker_init)
	LOAD_SYM(enchant_broker_free)
	LOAD_SYM(enchant_broker_free_dict)
	LOAD_SYM(enchant_broker_list_dicts)
	LOAD_SYM(enchant_broker_request_dict)
	LOAD_SYM(enchant_dict_add_to_personal)
	LOAD_SYM(enchant_dict_add_to_session)
	LOAD_SYM(enchant_dict_check)
	LOAD_SYM(enchant_dict_describe)
	LOAD_SYM(enchant_dict_free_suggestions)
	LOAD_SYM(enchant_dict_suggest)

#undef LOAD_SYM

	have_enchant = true;
}

static void
spell_init_broker(void)
{
	if (!have_enchant || spell_broker)
		return;

	spell_broker = enchant_broker_init();
	if (!spell_broker)
		return;

	// Parse configured languages from prefs.hex_text_spell_langs
	char *langs_copy = g_strdup(prefs.hex_text_spell_langs);
	char **lang_array = g_strsplit_set(langs_copy, ", \t", -1);

	for (int i = 0; lang_array && lang_array[i]; i++)
	{
		const char *lang = lang_array[i];
		if (!lang || !*lang)
			continue;

		EnchantDict *dict = enchant_broker_request_dict(spell_broker, lang);
		if (dict)
		{
			spell_dicts.push_back(dict);
			// Add common IRC terms to session dictionary
			enchant_dict_add_to_session(dict, "HexChat", -1);
			enchant_dict_add_to_session(dict, "FlexChat", -1);
			enchant_dict_add_to_session(dict, "IRC", -1);
		}
	}

	g_strfreev(lang_array);
	g_free(langs_copy);

	// If no dictionaries loaded, try "en" as fallback
	if (spell_dicts.empty())
	{
		EnchantDict *dict = enchant_broker_request_dict(spell_broker, "en");
		if (dict)
			spell_dicts.push_back(dict);
	}
}

static void
spell_cleanup(void)
{
	if (spell_broker)
	{
		for (auto dict : spell_dicts)
			enchant_broker_free_dict(spell_broker, dict);
		spell_dicts.clear();
		enchant_broker_free(spell_broker);
		spell_broker = nullptr;
	}
	spell_session_ignores.clear();
}

static bool
spell_check_word(const char *word, size_t len)
{
	if (!have_enchant || spell_dicts.empty() || !word || len == 0)
		return true; // assume correct if no spell checking

	// Skip URLs, nicks starting with special chars, numbers
	if (len >= 4 && (strncasecmp(word, "http", 4) == 0 ||
	                 strncasecmp(word, "ftp:", 4) == 0 ||
	                 strncasecmp(word, "irc:", 4) == 0))
		return true;

	// Skip words that start with non-alphabetic characters
	if (!g_unichar_isalpha(g_utf8_get_char(word)))
		return true;

	// Check session ignores
	std::string w(word, len);
	if (spell_session_ignores.find(w) != spell_session_ignores.end())
		return true;

	// Check against all loaded dictionaries
	for (auto dict : spell_dicts)
	{
		if (enchant_dict_check(dict, word, len) == 0)
			return true; // word is correct in at least one dictionary
	}

	return false; // misspelled
}

static std::vector<std::string>
spell_get_suggestions(const char *word, size_t len)
{
	std::vector<std::string> result;
	if (!have_enchant || spell_dicts.empty() || !word || len == 0)
		return result;

	for (auto dict : spell_dicts)
	{
		size_t n_suggs = 0;
		char **suggestions = enchant_dict_suggest(dict, word, len, &n_suggs);
		if (suggestions)
		{
			for (size_t i = 0; i < n_suggs && result.size() < 10; i++)
			{
				// Avoid duplicates
				std::string s = suggestions[i];
				bool found = false;
				for (auto &existing : result)
				{
					if (existing == s) { found = true; break; }
				}
				if (!found)
					result.push_back(s);
			}
			enchant_dict_free_suggestions(dict, suggestions);
		}
	}
	return result;
}

static void
spell_add_to_dictionary(const char *word)
{
	if (!have_enchant || spell_dicts.empty() || !word || !*word)
		return;

	for (auto dict : spell_dicts)
		enchant_dict_add_to_personal(dict, word, -1);
}

static void
spell_ignore_word(const char *word)
{
	if (!word || !*word)
		return;
	spell_session_ignores.insert(word);

	if (have_enchant && !spell_dicts.empty())
	{
		for (auto dict : spell_dicts)
			enchant_dict_add_to_session(dict, word, -1);
	}
}

// Structure to hold word positions for spell checking
struct WordSpan
{
	int start;
	int end;
	bool misspelled;
};

static std::vector<WordSpan>
spell_find_words(const char *text)
{
	std::vector<WordSpan> words;
	if (!text || !*text)
		return words;

	const char *p = text;
	while (*p)
	{
		// Skip non-word characters
		while (*p && !g_unichar_isalpha(g_utf8_get_char(p)))
			p = g_utf8_next_char(p);

		if (!*p)
			break;

		const char *word_start = p;

		// Find end of word
		while (*p && (g_unichar_isalpha(g_utf8_get_char(p)) ||
		              g_utf8_get_char(p) == '\'' ||
		              g_utf8_get_char(p) == '-'))
			p = g_utf8_next_char(p);

		const char *word_end = p;
		size_t len = word_end - word_start;

		if (len > 0)
		{
			WordSpan ws;
			ws.start = word_start - text;
			ws.end = word_end - text;
			ws.misspelled = !spell_check_word(word_start, len);
			words.push_back(ws);
		}
	}
	return words;
}

// Custom input widget with spell checking support
class SpellInput : public Fl_Input
{
public:
	SpellInput(int X, int Y, int W, int H, const char *L = nullptr)
		: Fl_Input(X, Y, W, H, L), mark_pos(-1)
	{
	}

	int handle(int event) override
	{
		if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE)
		{
			// Find clicked position
			int mx = Fl::event_x() - x();
			// Approximate character position
			const char *val = value();
			if (!val || !*val)
				return Fl_Input::handle(event);

			// Get approximate position by measuring text width
			fl_font(textfont(), textsize());
			int pos = 0;
			int accum = 4; // left padding
			const char *p = val;
			while (*p && accum < mx)
			{
				int cw = (int)fl_width(p, g_utf8_next_char(p) - p);
				if (accum + cw / 2 > mx)
					break;
				accum += cw;
				p = g_utf8_next_char(p);
				pos++;
			}

			mark_pos = pos;

			// Check if we clicked on a misspelled word
			std::string word = get_word_at(pos);
			if (!word.empty() && !spell_check_word(word.c_str(), word.length()))
			{
				show_spell_menu(word);
				return 1;
			}
		}
		return Fl_Input::handle(event);
	}

	void draw() override
	{
		Fl_Input::draw();

		// Draw misspelled word underlines if spell checking is enabled
		if (!have_enchant || !prefs.hex_gui_input_spell)
			return;

		const char *val = value();
		if (!val || !*val)
			return;

		auto words = spell_find_words(val);
		if (words.empty())
			return;

		fl_font(textfont(), textsize());
		int baseline = y() + h() - 6;

		for (auto &ws : words)
		{
			if (!ws.misspelled)
				continue;

			// Calculate pixel positions for the word
			int x1 = x() + 4;
			if (ws.start > 0)
				x1 += (int)fl_width(val, ws.start);

			int x2 = x() + 4 + (int)fl_width(val, ws.end);

			// Draw red wavy underline
			fl_color(FL_RED);
			int wave_y = baseline;
			for (int px = x1; px < x2; px += 2)
			{
				int ny = wave_y + ((px / 2) % 2 ? 1 : -1);
				fl_line(px, wave_y, px + 2, ny);
				wave_y = ny;
			}
		}
	}

private:
	int mark_pos;

	std::string get_word_at(int char_pos)
	{
		const char *val = value();
		if (!val || !*val)
			return "";

		// Convert char position to byte position
		const char *p = val;
		for (int i = 0; i < char_pos && *p; i++)
			p = g_utf8_next_char(p);

		if (!*p)
			return "";

		// Find word boundaries
		const char *word_start = p;
		while (word_start > val)
		{
			const char *prev = g_utf8_prev_char(word_start);
			gunichar c = g_utf8_get_char(prev);
			if (!g_unichar_isalpha(c) && c != '\'' && c != '-')
				break;
			word_start = prev;
		}

		const char *word_end = p;
		while (*word_end)
		{
			gunichar c = g_utf8_get_char(word_end);
			if (!g_unichar_isalpha(c) && c != '\'' && c != '-')
				break;
			word_end = g_utf8_next_char(word_end);
		}

		if (word_end <= word_start)
			return "";

		return std::string(word_start, word_end - word_start);
	}

	void show_spell_menu(const std::string &word)
	{
		auto suggestions = spell_get_suggestions(word.c_str(), word.length());

		// Build menu dynamically
		std::vector<Fl_Menu_Item> items;

		// Add suggestions
		std::vector<std::string> suggestion_storage = suggestions; // Keep strings alive
		for (size_t i = 0; i < suggestion_storage.size() && i < 10; i++)
		{
			Fl_Menu_Item item = {suggestion_storage[i].c_str(), 0, nullptr, (void *)(intptr_t)i};
			items.push_back(item);
		}

		if (items.empty())
		{
			Fl_Menu_Item item = {_("(no suggestions)"), 0, nullptr, nullptr, FL_MENU_INACTIVE};
			items.push_back(item);
		}

		// Separator
		Fl_Menu_Item sep = {nullptr, 0, nullptr, nullptr, FL_MENU_DIVIDER};
		items.push_back(sep);

		// Add to dictionary
		Fl_Menu_Item add_item = {_("Add to Dictionary"), 0, nullptr, (void *)-1};
		items.push_back(add_item);

		// Ignore
		Fl_Menu_Item ignore_item = {_("Ignore All"), 0, nullptr, (void *)-2};
		items.push_back(ignore_item);

		// Terminator
		Fl_Menu_Item term = {nullptr};
		items.push_back(term);

		const Fl_Menu_Item *picked = items.data()->popup(Fl::event_x(), Fl::event_y());
		if (!picked)
			return;

		intptr_t idx = (intptr_t)picked->user_data();

		if (idx == -1)
		{
			// Add to dictionary
			spell_add_to_dictionary(word.c_str());
			redraw();
		}
		else if (idx == -2)
		{
			// Ignore
			spell_ignore_word(word.c_str());
			redraw();
		}
		else if (idx >= 0 && idx < (intptr_t)suggestion_storage.size())
		{
			// Replace word with suggestion
			replace_word_at_mark(word, suggestion_storage[idx]);
		}
	}

	void replace_word_at_mark(const std::string &old_word, const std::string &new_word)
	{
		const char *val = value();
		if (!val)
			return;

		// Find the word in the text
		const char *found = strstr(val, old_word.c_str());
		if (!found)
			return;

		int start = found - val;
		int end = start + old_word.length();

		// Build new string
		std::string newval;
		newval.append(val, start);
		newval.append(new_word);
		newval.append(val + end);

		value(newval.c_str());
		position(start + new_word.length());
		redraw();
	}
};

// ============================================================================
// Color Palette System
// ============================================================================
// Customizable color palette for mIRC colors and UI elements.
// Colors can be edited via the palette dialog and saved to colors.conf.

// Color indices (matching GTK version)
#define COL_MARK_FG 32
#define COL_MARK_BG 33
#define COL_FG 34
#define COL_BG 35
#define COL_MARKER 36
#define COL_NEW_DATA 37
#define COL_HILIGHT 38
#define COL_NEW_MSG 39
#define COL_AWAY 40
#define COL_SPELL 41
#define MAX_COL 41

// Color palette storage (RGB values 0-255)
struct PaletteColor
{
	unsigned char r, g, b;
};

// Default mIRC colors and UI colors
static PaletteColor palette_colors[MAX_COL + 1] = {
	// mIRC colors 0-15
	{211, 215, 207},  // 0 white
	{46, 52, 54},     // 1 black
	{52, 101, 164},   // 2 blue
	{78, 154, 6},     // 3 green
	{204, 0, 0},      // 4 red
	{143, 57, 2},     // 5 brown/maroon
	{92, 53, 102},    // 6 purple
	{206, 92, 0},     // 7 orange
	{196, 160, 0},    // 8 yellow
	{115, 210, 22},   // 9 light green
	{17, 168, 121},   // 10 cyan/teal
	{88, 161, 157},   // 11 light cyan
	{87, 121, 158},   // 12 light blue
	{160, 66, 101},   // 13 pink/light purple
	{85, 87, 83},     // 14 grey
	{136, 138, 133},  // 15 light grey
	// mIRC colors 16-31 (duplicates of 0-15 for extended palette)
	{211, 215, 207}, {46, 52, 54}, {52, 101, 164}, {78, 154, 6},
	{204, 0, 0}, {143, 57, 2}, {92, 53, 102}, {206, 92, 0},
	{196, 160, 0}, {115, 210, 22}, {17, 168, 121}, {88, 161, 157},
	{87, 121, 158}, {160, 66, 101}, {85, 87, 83}, {136, 138, 133},
	// Special colors 32-41
	{211, 215, 207},  // 32 COL_MARK_FG (selection foreground)
	{32, 74, 135},    // 33 COL_MARK_BG (selection background)
	{37, 41, 43},     // 34 COL_FG (text foreground)
	{250, 250, 248},  // 35 COL_BG (text background)
	{143, 57, 2},     // 36 COL_MARKER (marker line)
	{52, 101, 164},   // 37 COL_NEW_DATA (new data tab)
	{78, 154, 6},     // 38 COL_HILIGHT (highlight tab)
	{206, 92, 0},     // 39 COL_NEW_MSG (new message tab)
	{136, 138, 133},  // 40 COL_AWAY (away user)
	{164, 0, 0},      // 41 COL_SPELL (spell error)
};

// Backup of default colors for reset
static PaletteColor default_palette_colors[MAX_COL + 1];
static bool palette_defaults_saved = false;

static void
palette_save_defaults(void)
{
	if (!palette_defaults_saved)
	{
		memcpy(default_palette_colors, palette_colors, sizeof(palette_colors));
		palette_defaults_saved = true;
	}
}

static Fl_Color
palette_get_fl_color(int index)
{
	if (index < 0 || index > MAX_COL)
		return FL_FOREGROUND_COLOR;
	PaletteColor &c = palette_colors[index];
	return fl_rgb_color(c.r, c.g, c.b);
}

static void
palette_set_color(int index, unsigned char r, unsigned char g, unsigned char b)
{
	if (index < 0 || index > MAX_COL)
		return;
	palette_colors[index].r = r;
	palette_colors[index].g = g;
	palette_colors[index].b = b;
}

static void
palette_load(void)
{
	palette_save_defaults();

	char *path = g_build_filename(get_xdir(), "colors.conf", nullptr);
	FILE *fp = fopen(path, "r");
	g_free(path);

	if (!fp)
		return;

	char line[256];
	while (fgets(line, sizeof(line), fp))
	{
		int idx, r, g, b;
		if (sscanf(line, "color_%d = %d %d %d", &idx, &r, &g, &b) == 4)
		{
			// Map special colors (256+) to our indices (32+)
			if (idx >= 256)
				idx = 32 + (idx - 256);
			if (idx >= 0 && idx <= MAX_COL)
			{
				palette_colors[idx].r = (unsigned char)r;
				palette_colors[idx].g = (unsigned char)g;
				palette_colors[idx].b = (unsigned char)b;
			}
		}
	}
	fclose(fp);
}

static void
palette_save(void)
{
	char *path = g_build_filename(get_xdir(), "colors.conf", nullptr);
	FILE *fp = fopen(path, "w");
	g_free(path);

	if (!fp)
		return;

	// Save mIRC colors 0-31
	for (int i = 0; i < 32; i++)
	{
		fprintf(fp, "color_%d = %d %d %d\n", i,
		        palette_colors[i].r, palette_colors[i].g, palette_colors[i].b);
	}

	// Save special colors (mapped to 256+)
	for (int i = 32; i <= MAX_COL; i++)
	{
		fprintf(fp, "color_%d = %d %d %d\n", 256 + (i - 32),
		        palette_colors[i].r, palette_colors[i].g, palette_colors[i].b);
	}

	fclose(fp);
}

static void
palette_reset(void)
{
	if (palette_defaults_saved)
		memcpy(palette_colors, default_palette_colors, sizeof(palette_colors));
}

// Forward declarations needed by inline browser classes
static void chanlist_join_cb (Fl_Widget *, void *);
static session *find_session_by_tab (Fl_Group *grp);
static void close_tab_cb (Fl_Widget *, void *data);
static void session_tree_rebuild (void);
static void session_tree_cb (Fl_Widget *, void *);

static bool session_tree_updating = false;
static void show_session_content (session *sess);

struct SessionUI
{
	Fl_Group *tab {nullptr};
	Fl_Text_Display *display {nullptr};
	Fl_Text_Buffer *buffer {nullptr};
	Fl_Text_Buffer *style_buffer {nullptr};
	Fl_Box *topic {nullptr};
	Fl_Button *topic_btn {nullptr};
	Fl_Hold_Browser *user_browser {nullptr};
	Fl_Group *toolbar {nullptr};
	Fl_Button *op_btn {nullptr};
	Fl_Button *voice_btn {nullptr};
	Fl_Button *ban_btn {nullptr};
	Fl_Button *kick_btn {nullptr};
	std::map<std::string, std::string> users;
	bool userlist_dirty {false};
};

static SessionUI *ensure_session_ui (session *sess);
static void tab_changed_cb (Fl_Widget *, void *);

static bool
sess_can_manage (session *sess)
{
	if (!sess || !sess->me)
		return false;
	char p = sess->me->prefix[0];
	return sess->me->op || sess->me->hop || p == '@' || p == '&' || p == '~' || p == '%';
}

static bool
sess_has_voice (session *sess)
{
	if (!sess || !sess->me)
		return false;
	char p = sess->me->prefix[0];
	return sess->me->voice || sess->me->op || sess->me->hop || p == '+' || p == '@' || p == '&' || p == '~' || p == '%';
}

static std::string
user_nick_for_line (SessionUI *ui, int line)
{
	if (!ui || line <= 0)
		return "";
	int idx = 1;
	for (auto &entry : ui->users)
	{
		if (idx == line)
			return entry.first;
		idx++;
	}
	return "";
}

class UserBrowser : public Fl_Hold_Browser
{
public:
	UserBrowser (int X, int Y, int W, int H, session *s)
	: Fl_Hold_Browser (X, Y, W, H), sess (s), owner (nullptr)
	{
	}

	void set_owner (SessionUI *ui)
	{
		owner = ui;
	}

	int handle (int event) override
	{
		if (event == FL_PUSH && Fl::event_button () == FL_RIGHT_MOUSE)
		{
			int handled = Fl_Hold_Browser::handle (event);
			int line = value ();
			show_context_menu (line);
			return handled;
		}
		if (event == FL_RELEASE && Fl::event_button () == FL_LEFT_MOUSE && Fl::event_clicks ())
		{
			int handled = Fl_Hold_Browser::handle (event);
			int line = value ();
			start_query (line);
			return handled;
		}
		return Fl_Hold_Browser::handle (event);
	}

protected:
	void item_draw (void *item, int X, int Y, int W, int H) const override
	{
		const char *txt = item ? item_text (item) : nullptr;
		Fl_Color col = textcolor ();
		Fl_Font font = textfont ();
		bool is_op = false;
		if (txt && *txt)
		{
			char prefix = *txt;
			if (prefix == '@' || prefix == '&' || prefix == '~')
			{
				col = FL_RED;
				font = FL_COURIER_BOLD;
				is_op = true;
			}
			else if (prefix == '+')
			{
				col = FL_DARK_GREEN;
				font = FL_COURIER;
			}
		}
		fl_color (col);
		fl_font (font, textsize ());
		Fl_Hold_Browser::item_draw (item, X, Y, W, H);
		if (is_op)
		{
			int sz = 3;
			int cy = Y + H / 2 - sz / 2;
			fl_color (FL_DARK_GREEN);
			fl_rectf (X + 2, cy, sz, sz);
		}
	}

private:
	void show_context_menu (int line)
	{
		if (!owner || !sess)
			return;

		std::string nick = user_nick_for_line (owner, line);
		if (nick.empty ())
			return;

		static Fl_Menu_Item items[] = {
			{_("Query"), 0, nullptr, (void *)"QUERY"},
			{_("Whois"), 0, nullptr, (void *)"WHOIS"},
			{_("Op"), 0, nullptr, (void *)"MODE +o"},
			{_("DeOp"), 0, nullptr, (void *)"MODE -o"},
			{_("Voice"), 0, nullptr, (void *)"MODE +v"},
			{_("DeVoice"), 0, nullptr, (void *)"MODE -v"},
			{_("Kick"), 0, nullptr, (void *)"KICK"},
			{nullptr}
		};

		const Fl_Menu_Item *picked = items->popup (Fl::event_x (), Fl::event_y (), nullptr, nullptr, 0);
		if (!picked || !picked->user_data ())
			return;

		const char *cmd = static_cast<const char *>(picked->user_data ());
		char buf[512];

		if (strcmp (cmd, "QUERY") == 0)
			g_snprintf (buf, sizeof buf, "QUERY %s", nick.c_str ());
		else if (strcmp (cmd, "WHOIS") == 0)
			g_snprintf (buf, sizeof buf, "WHOIS %s", nick.c_str ());
		else if (strcmp (cmd, "KICK") == 0)
		{
			const char *chan = (sess && sess->channel[0]) ? sess->channel : "";
			g_snprintf (buf, sizeof buf, "KICK %s %s", chan, nick.c_str ());
		}
		else
		{
			g_snprintf (buf, sizeof buf, "%s %s", cmd, nick.c_str ());
		}

		handle_command (sess, buf, FALSE);
	}

	void start_query (int line)
	{
		if (!sess)
			return;
		std::string nick = user_nick_for_line (owner, line);
		if (nick.empty ())
			return;
		char buf[256];
		g_snprintf (buf, sizeof buf, "QUERY %s", nick.c_str ());
		handle_command (sess, buf, FALSE);
	}

	session *sess;
	SessionUI *owner;
};

class ChannelListBrowser : public Fl_Select_Browser
{
public:
	ChannelListBrowser (int X, int Y, int W, int H, server *s)
	: Fl_Select_Browser (X, Y, W, H), serv (s) {}

	int handle (int event) override
	{
		if (event == FL_RELEASE && Fl::event_button () == FL_LEFT_MOUSE && Fl::event_clicks ())
		{
			int handled = Fl_Select_Browser::handle (event);
			if (serv)
				chanlist_join_cb (this, serv);
			return handled;
		}
		return Fl_Select_Browser::handle (event);
	}

private:
	server *serv;
};

struct CloseRect
{
	int x, y, w, h;
};

static bool looks_like_url (const char *p);

class ClosableTabs : public Fl_Tabs
{
public:
	ClosableTabs (int X, int Y, int W, int H, const char *L = 0)
	: Fl_Tabs (X, Y, W, H, L) {}

	int handle (int ev) override
	{
		if (ev == FL_PUSH && Fl::event_button () == FL_LEFT_MOUSE)
		{
			int mx = Fl::event_x ();
			int my = Fl::event_y ();
			Fl_Widget *child = which (mx, my);
			if (child)
			{
				auto it = close_rects.find (child);
				if (it != close_rects.end ())
				{
					CloseRect r = it->second;
					if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h)
					{
						// Invoke the same close logic as the in-tab button
						session *sess = find_session_by_tab (static_cast<Fl_Group *>(child));
						if (sess)
							close_tab_cb (nullptr, sess);
						return 1;
					}
				}
			}
		}
		return Fl_Tabs::handle (ev);
	}

	void draw () override
	{
		update_close_rects ();
		Fl_Tabs::draw ();
		// Draw close glyphs
		fl_color (FL_DARK3);
		for (auto &pair : close_rects)
		{
			const CloseRect &r = pair.second;
			fl_rectf (r.x, r.y, r.w, r.h, FL_BACKGROUND_COLOR);
			fl_color (FL_DARK_RED);
			fl_draw ("x", r.x + 3, r.y + r.h - 3);
			fl_color (FL_DARK3);
		}
	}

private:
	void update_close_rects ()
	{
		close_rects.clear ();
		int tx = x () + 4;
		int ty = y () + 4;
		int th = 24;
		for (int i = 0; i < children (); i++)
		{
			Fl_Widget *c = child (i);
			const char *lbl = c->label () ? c->label () : "";
			int lw = static_cast<int>(fl_width (lbl));
			int tw = lw + 30; // padding for close glyph
			CloseRect r {tx + tw - 18, ty + 5, 12, 12};
			close_rects[c] = r;
			tx += tw + 6;
		}
	}

	std::map<Fl_Widget *, CloseRect> close_rects;
};

class ChatDisplay : public Fl_Text_Display
{
public:
	ChatDisplay (int X, int Y, int W, int H) : Fl_Text_Display (X, Y, W, H) {}

	int handle (int ev) override
	{
		if (ev == FL_RELEASE)
		{
			int pos = xy_to_position (Fl::event_x (), Fl::event_y ());
			if (pos >= 0)
			{
				if (Fl::event_button () == FL_LEFT_MOUSE &&
					(Fl::event_clicks () || (Fl::event_state (FL_CTRL) != 0)))
				{
					open_url_at (pos, false);
					return 1;
				}
				if (Fl::event_button () == FL_RIGHT_MOUSE)
				{
					open_url_at (pos, true);
					return 1;
				}
			}
		}
		return Fl_Text_Display::handle (ev);
	}

private:
	void open_url_at (int pos, bool copy_only)
	{
		Fl_Text_Buffer *buf = buffer ();
		if (!buf)
			return;
		int len = buf->length ();
		if (pos < 0 || pos >= len)
			return;

		int start = pos;
		int end = pos;
		while (start > 0)
		{
			char c = buf->char_at (start - 1);
			if (c == '\n' || c == ' ' || c == '\t' || c == '<')
				break;
			start--;
		}
		while (end < len)
		{
			char c = buf->char_at (end);
			if (c == '\n' || c == ' ' || c == '\t' || c == '>' || c == ')')
				break;
			end++;
		}
		if (end <= start)
			return;
		std::string candidate = buf->text_range (start, end);
		if (candidate.empty ())
			return;
		if (looks_like_url (candidate.c_str ()))
		{
			if (copy_only)
				Fl::copy (candidate.c_str (), (int)candidate.size (), 1);
			else
				fe_open_url (candidate.c_str ());
		}
	}
};

// DCC window structure
struct DCCWindow
{
	Fl_Window *window {nullptr};
	Fl_Select_Browser *list {nullptr};
	Fl_Button *abort_btn {nullptr};
	Fl_Button *accept_btn {nullptr};
	Fl_Button *resume_btn {nullptr};
	Fl_Button *clear_btn {nullptr};
	Fl_Box *file_label {nullptr};
	Fl_Box *address_label {nullptr};
	int view_mode {3}; // 1=download, 2=upload, 3=both
};

// Channel list window structure
struct ChanListWindow
{
	Fl_Window *window {nullptr};
	Fl_Select_Browser *list {nullptr};
	Fl_Input *filter_input {nullptr};
	Fl_Spinner *min_users {nullptr};
	Fl_Spinner *max_users {nullptr};
	Fl_Check_Button *match_channel {nullptr};
	Fl_Check_Button *match_topic {nullptr};
	Fl_Button *refresh_btn {nullptr};
	Fl_Button *join_btn {nullptr};
	Fl_Button *save_btn {nullptr};
	Fl_Box *info_label {nullptr};
	server *serv {nullptr};
	int channels_found {0};
	int channels_shown {0};
	int users_found {0};
	int users_shown {0};
};

// Menu entry tracking for dynamic menus
struct MenuEntry
{
	std::string path;
	std::string label;
	std::string cmd;
	int pos {0};
	bool is_main {false};
	bool enabled {true};
};

// Server List window structure
struct ServerListWindow
{
	Fl_Window *window {nullptr};
	Fl_Hold_Browser *network_list {nullptr};
	Fl_Hold_Browser *server_list {nullptr};
	Fl_Input *nick1_input {nullptr};
	Fl_Input *nick2_input {nullptr};
	Fl_Input *nick3_input {nullptr};
	Fl_Input *username_input {nullptr};
	Fl_Input *realname_input {nullptr};
	Fl_Check_Button *auto_connect {nullptr};
	Fl_Check_Button *use_ssl {nullptr};
	Fl_Check_Button *use_global {nullptr};
	Fl_Check_Button *cycle_servers {nullptr};
	Fl_Input *password_input {nullptr};
	Fl_Choice *login_type {nullptr};
	Fl_Input *sasl_user {nullptr};
	Fl_Input *sasl_pass {nullptr};
	Fl_Input *connect_cmd {nullptr};
	Fl_Input *encoding_input {nullptr};
	Fl_Hold_Browser *fav_channels {nullptr};
	Fl_Input *fav_key_input {nullptr};
	Fl_Check_Button *allow_invalid {nullptr};
	Fl_Check_Button *use_proxy {nullptr};
	Fl_Button *connect_btn {nullptr};
	ircnet *selected_net {nullptr};
	session *sess {nullptr};
};

// Preferences window structure
struct PrefsWindow
{
	Fl_Window *window {nullptr};
	Fl_Tabs *tabs {nullptr};
	// Interface tab
	Fl_Input *font_input {nullptr};
	Fl_Check_Button *show_timestamps {nullptr};
	Fl_Input *timestamp_format {nullptr};
	Fl_Check_Button *show_topic {nullptr};
	Fl_Check_Button *show_userlist {nullptr};
	Fl_Check_Button *colored_nicks {nullptr};
	Fl_Check_Button *enable_spell {nullptr};
	Fl_Input *spell_langs {nullptr};
	// Chatting tab
	Fl_Input *nick1 {nullptr};
	Fl_Input *nick2 {nullptr};
	Fl_Input *nick3 {nullptr};
	Fl_Input *username {nullptr};
	Fl_Input *realname {nullptr};
	Fl_Input *quit_msg {nullptr};
	Fl_Input *part_msg {nullptr};
	Fl_Input *away_msg {nullptr};
	// Network tab
	Fl_Check_Button *auto_reconnect {nullptr};
	Fl_Spinner *reconnect_delay {nullptr};
	Fl_Check_Button *use_proxy {nullptr};
	Fl_Choice *proxy_type {nullptr};
	Fl_Input *proxy_host {nullptr};
	Fl_Spinner *proxy_port {nullptr};
	// DCC tab
	Fl_Input *dcc_dir {nullptr};
	Fl_Input *dcc_completed_dir {nullptr};
	Fl_Spinner *dcc_port_first {nullptr};
	Fl_Spinner *dcc_port_last {nullptr};
	Fl_Check_Button *dcc_auto_accept {nullptr};
	// Logging tab
	Fl_Check_Button *enable_logging {nullptr};
	Fl_Input *log_dir {nullptr};
	Fl_Input *log_timestamp {nullptr};
	Fl_Button *log_browse {nullptr};
	// Alerts tab
	Fl_Check_Button *beep_on_msg {nullptr};
	Fl_Check_Button *beep_on_hilight {nullptr};
	Fl_Check_Button *beep_on_priv {nullptr};
	Fl_Check_Button *flash_on_msg {nullptr};
	Fl_Check_Button *flash_on_hilight {nullptr};
	Fl_Check_Button *flash_on_priv {nullptr};
};

// Raw Log window structure
struct RawLogWindow
{
	Fl_Window *window {nullptr};
	Fl_Text_Display *display {nullptr};
	Fl_Text_Buffer *buffer {nullptr};
	Fl_Check_Button *inbound {nullptr};
	Fl_Check_Button *outbound {nullptr};
	server *serv {nullptr};
};

// URL Grabber window structure
struct URLGrabberWindow
{
	Fl_Window *window {nullptr};
	Fl_Select_Browser *list {nullptr};
	Fl_Button *open_btn {nullptr};
	Fl_Button *copy_btn {nullptr};
	Fl_Button *clear_btn {nullptr};
	Fl_Button *save_btn {nullptr};
};

// Notify List window structure
struct NotifyListWindow
{
	Fl_Window *window {nullptr};
	Fl_Select_Browser *list {nullptr};
	Fl_Input *nick_input {nullptr};
	Fl_Input *network_input {nullptr};
	Fl_Button *add_btn {nullptr};
	Fl_Button *remove_btn {nullptr};
};

// Ignore List window structure
struct IgnoreListWindow
{
	Fl_Window *window {nullptr};
	Fl_Select_Browser *list {nullptr};
	Fl_Input *mask_input {nullptr};
	Fl_Check_Button *ignore_priv {nullptr};
	Fl_Check_Button *ignore_notice {nullptr};
	Fl_Check_Button *ignore_chan {nullptr};
	Fl_Check_Button *ignore_ctcp {nullptr};
	Fl_Check_Button *ignore_dcc {nullptr};
	Fl_Check_Button *ignore_invite {nullptr};
	Fl_Button *add_btn {nullptr};
	Fl_Button *remove_btn {nullptr};
};

// Ban List window structure
struct BanListWindow
{
	Fl_Window *window {nullptr};
	Fl_Select_Browser *list {nullptr};
	Fl_Input *mask_input {nullptr};
	Fl_Button *add_btn {nullptr};
	Fl_Button *remove_btn {nullptr};
	Fl_Button *refresh_btn {nullptr};
	session *sess {nullptr};
};

// Join Channel dialog structure (per-server)
struct JoinChannelDialog
{
	Fl_Window *window {nullptr};
	Fl_Input *channel_input {nullptr};
	Fl_Input *key_input {nullptr};
	Fl_Hold_Browser *history_list {nullptr};
	Fl_Check_Button *show_on_connect {nullptr};
	Fl_Button *join_btn {nullptr};
	Fl_Button *chanlist_btn {nullptr};
	server *serv {nullptr};
};

// Input history for command recall
static std::vector<std::string> input_history;
static int history_pos = -1;
static const int MAX_HISTORY = 100;

// Global windows
static Fl_Window *main_win = nullptr;
static Fl_Menu_Bar *menu_bar = nullptr;
static ClosableTabs *tab_widget = nullptr; // unused now
static Fl_Group *content_stack = nullptr;
static Fl_Tree *session_tree = nullptr;
static SpellInput *input_box = nullptr;
static Fl_Button *send_button = nullptr;
static Fl_Box *status_bar = nullptr;
static Fl_Progress *lag_indicator = nullptr;
static Fl_Progress *throttle_indicator = nullptr;
static Fl_Box *user_count_label = nullptr;
static std::map<session *, SessionUI> session_ui_map;
static std::map<server *, ChanListWindow> chanlist_windows;
static std::map<server *, RawLogWindow> rawlog_windows;
static std::map<session *, BanListWindow> banlist_windows;
static std::map<server *, JoinChannelDialog> join_dialogs;
static DCCWindow dcc_file_window;
static DCCWindow dcc_chat_window;
static ServerListWindow servlist_window;
static PrefsWindow prefs_window;
static URLGrabberWindow url_grabber_window;
static NotifyListWindow notify_window;
static IgnoreListWindow ignore_window;
static std::list<MenuEntry> dynamic_menus;
static bool fltk_debug = false;
static bool userlist_idle_scheduled = false;

static void
debug_log (const char *fmt, ...)
{
	if (!fltk_debug || !fmt)
		return;

	va_list ap;
	va_start (ap, fmt);
	fprintf (stderr, "[fltk-debug] ");
	vfprintf (stderr, fmt, ap);
	fprintf (stderr, "\n");
	fflush (stderr);
	va_end (ap);
}

static void
parse_font_spec (const char *spec, std::string &name, int &size)
{
	name.clear ();
	size = 12;
	if (!spec || !*spec)
		return;

	std::string s (spec);
	// trim
	while (!s.empty () && std::isspace ((unsigned char)s.back ()))
		s.pop_back ();
	size_t pos = s.find_last_of (' ');
	if (pos != std::string::npos)
	{
		std::string tail = s.substr (pos + 1);
		bool all_digits = true;
		for (char c : tail)
		{
			if (!std::isdigit ((unsigned char)c))
			{
				all_digits = false;
				break;
			}
		}
		if (all_digits)
		{
			size = atoi (tail.c_str ());
			s = s.substr (0, pos);
		}
	}
	while (!s.empty () && std::isspace ((unsigned char)s.back ()))
		s.pop_back ();
	name = s;
	if (size <= 0)
		size = 12;
}

static void
apply_font_to_widgets (const std::string &name, int size)
{
	const char *fname = name.empty () ? "DejaVu Sans Mono" : name.c_str ();
	if (size <= 0)
		size = 12;

	Fl::set_font (FL_COURIER, fname);

	if (input_box)
	{
		input_box->textfont (FL_COURIER);
		input_box->textsize (size);
	}
	if (send_button)
		send_button->labelsize (size);
}

// Build a style table for the text display that approximates GTK rendering.
// Layout (index = style char - 'A'):
// 0  : default
// 1  : action (italic)
// 2  : CTCP (bold)
// 3-18  : normal mIRC fg colors 0-15
// 19-37 : bold versions (default + mIRC colors)
// 38-56 : underline versions (default + mIRC colors)
// 57    : hyperlink (blue + underline)
static void
build_style_table (Fl_Text_Display::Style_Table_Entry *table, int fsize)
{
	Fl_Color mirc_colors[16] = {
		FL_WHITE,       // 0 white
		FL_BLACK,       // 1 black
		FL_BLUE,        // 2 navy
		FL_DARK_GREEN,  // 3 green
		FL_RED,         // 4 red
		FL_DARK_RED,    // 5 brown
		FL_MAGENTA,     // 6 purple
		FL_DARK_YELLOW, // 7 orange/olive
		FL_YELLOW,      // 8 yellow
		FL_GREEN,       // 9 light green
		FL_CYAN,        // 10 cyan
		FL_DARK_CYAN,   // 11 light cyan/teal
		FL_DARK_BLUE,   // 12 light blue
		FL_DARK_MAGENTA,// 13 pink
		FL_DARK3,       // 14 gray
		FL_LIGHT2       // 15 light gray
	};

	int region = 19;
	table[0] = {FL_FOREGROUND_COLOR, FL_COURIER, fsize, 0};
	table[1] = {FL_DARK_GREEN, FL_COURIER_ITALIC, fsize, 0};
	table[2] = {FL_BLUE, FL_COURIER_BOLD, fsize, 0};
	for (int i = 0; i < 16; i++)
		table[3 + i] = {mirc_colors[i], FL_COURIER, fsize, 0};
	// Bold region
	table[19 + 0] = {FL_FOREGROUND_COLOR, FL_COURIER_BOLD, fsize, 0};
	for (int i = 0; i < 16; i++)
		table[19 + 3 + i] = {mirc_colors[i], FL_COURIER_BOLD, fsize, 0};
	// Underline region (FLTK lacks underline attr; reuse normal font but separate slot for future use)
	table[38 + 0] = {FL_FOREGROUND_COLOR, FL_COURIER, fsize, 0};
	for (int i = 0; i < 16; i++)
		table[38 + 3 + i] = {mirc_colors[i], FL_COURIER, fsize, 0};
	// Hyperlink style (blue slot)
	table[57] = {FL_BLUE, FL_COURIER, fsize, 0};
}

static bool
looks_like_url (const char *p)
{
	if (!p || !*p)
		return false;
	return g_ascii_strncasecmp (p, "http://", 7) == 0 ||
	       g_ascii_strncasecmp (p, "https://", 8) == 0 ||
	       g_ascii_strncasecmp (p, "ftp://", 6) == 0 ||
	       g_ascii_strncasecmp (p, "irc://", 6) == 0 ||
	       g_ascii_strncasecmp (p, "www.", 4) == 0;
}

// Forward declarations for dialog functions
static void servlist_open (session *sess);
static void prefs_open (void);
static void rawlog_open (server *serv);
static void rawlog_append (server *serv, const char *text, int outbound);
static void url_grabber_open (void);
static void notify_open (void);
static void ignore_open (void);
static void banlist_open (session *sess);
static void chanlist_join_cb (Fl_Widget *, void *);
static void servlist_network_select_cb (Fl_Widget *, void *);
static void session_tree_rebuild (void);

static void
set_status (const char *text)
{
	if (status_bar && text)
		status_bar->label (text);
}

static SessionUI *
find_ui_by_tab (Fl_Group *grp)
{
	for (auto &pair : session_ui_map)
	{
		if (pair.second.tab == grp)
			return &pair.second;
	}
	return nullptr;
}

static session *
find_session_by_tab (Fl_Group *grp)
{
	for (auto &pair : session_ui_map)
	{
		if (pair.second.tab == grp)
			return pair.first;
	}
	return nullptr;
}

static void
session_tree_rebuild (void)
{
	if (!session_tree)
		return;

	if (session_tree_updating)
		return;
	session_tree_updating = true;

	session_tree->clear ();
	Fl_Tree_Item *to_select = nullptr;

	for (auto &pair : session_ui_map)
	{
		session *sess = pair.first;
		if (!sess)
			continue;
		if (!sess->server)
			continue;
		const char *srv = _("server");
		if (sess->server && sess->server->servername[0])
			srv = sess->server->servername;
		const char *chan = (sess->channel[0]) ? sess->channel : srv;
		std::string path = std::string (srv) + "/" + chan;
		Fl_Tree_Item *item = session_tree->add (path.c_str ());
		if (item)
		{
			item->user_data (sess);
			if (sess == current_tab)
				to_select = item;
		}
	}

	if (to_select)
		session_tree->select_only (to_select);
	else if (session_tree->first ())
		session_tree->select_only (session_tree->first ());

	session_tree->redraw ();
	session_tree_updating = false;
}

static void
show_session_content (session *sess)
{
	if (!sess)
		return;
	SessionUI *ui = ensure_session_ui (sess);
	if (!ui)
		return;

	for (auto &pair : session_ui_map)
	{
		if (pair.second.tab)
			pair.second.tab->hide ();
	}
	if (ui->tab)
		ui->tab->show ();

	current_sess = sess;
	current_tab = sess;
	if (main_win)
	{
		const char *label = sess->channel[0] ? sess->channel : _("server");
		main_win->label (label);
	}
	session_tree_rebuild ();
}

// Toolbar button callbacks
static void toolbar_op_cb (Fl_Widget *, void *data)
{
	session *sess = static_cast<session *>(data);
	if (!sess) sess = current_sess;
	if (sess)
		handle_command (sess, (char *)"OP", FALSE);
}

static void toolbar_voice_cb (Fl_Widget *, void *data)
{
	session *sess = static_cast<session *>(data);
	if (!sess) sess = current_sess;
	if (sess)
		handle_command (sess, (char *)"VOICE", FALSE);
}

static void toolbar_ban_cb (Fl_Widget *, void *data)
{
	session *sess = static_cast<session *>(data);
	if (!sess) sess = current_sess;
	if (sess)
		handle_command (sess, (char *)"BAN", FALSE);
}

static void toolbar_kick_cb (Fl_Widget *, void *data)
{
	session *sess = static_cast<session *>(data);
	if (!sess) sess = current_sess;
	if (sess)
		handle_command (sess, (char *)"KICK", FALSE);
}

static void topic_edit_cb (Fl_Widget *, void *data)
{
	session *sess = static_cast<session *>(data);
	if (!sess) sess = current_sess;
	if (!sess)
		return;
	const char *cur = sess->topic ? sess->topic : "";
	const char *newtopic = fl_input (_("Set topic:"), cur);
	if (newtopic)
	{
		char buf[512];
		g_snprintf (buf, sizeof buf, "TOPIC %s", newtopic);
		handle_command (sess, buf, FALSE);
	}
}

static void close_tab_cb (Fl_Widget *, void *data)
{
	session *sess = static_cast<session *>(data);
	if (!sess) sess = current_sess;
	if (!sess)
		return;

	if (sess->channel[0])
		handle_command (sess, (char *)"PART", FALSE);
	else
		fe_close_window (sess);
}

static void
session_tree_cb (Fl_Widget *, void *)
{
	if (!session_tree)
		return;
	if (session_tree_updating)
		return;
	Fl_Tree_Item *item = session_tree->item_clicked ();
	if (!item)
		item = session_tree->first_selected_item ();
	if (!item)
		return;
	session *sess = static_cast<session *>(item->user_data ());
	if (!sess)
		return;
	show_session_content (sess);
}

static SessionUI *
ensure_session_ui (session *sess)
{
	auto it = session_ui_map.find (sess);
	if (it != session_ui_map.end ())
		return &it->second;

	if (!content_stack)
		return nullptr;

	const char *label = sess && sess->channel[0] ? sess->channel : _("server");
	int content_x = content_stack->x () + 10;
	int content_y = content_stack->y () + 5;
	int content_w = content_stack->w () - 20;
	int content_h = content_stack->h () - 10;

	Fl_Group *grp = new Fl_Group (content_x, content_y, content_w, content_h, label);

	// Topic line with edit/close controls
	Fl_Box *topic = new Fl_Box (content_x, content_y, content_w - 250, 24, "");
	topic->align (FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	topic->box (FL_THIN_DOWN_BOX);
	Fl_Button *topic_btn = new Fl_Button (content_x + content_w - 245, content_y, 60, 24, _("Edit"));
	topic_btn->callback (topic_edit_cb, sess);

	// Toolbar buttons (right side of topic line)
	int tbx = content_x + content_w - 190;
	Fl_Group *toolbar = new Fl_Group (tbx, content_y, 180, 24);
	Fl_Button *op_btn = new Fl_Button (tbx, content_y, 40, 24, "+o");
	op_btn->tooltip (_("Give Op"));
	op_btn->callback (toolbar_op_cb, sess);

	Fl_Button *voice_btn = new Fl_Button (tbx + 45, content_y, 40, 24, "+v");
	voice_btn->tooltip (_("Give Voice"));
	voice_btn->callback (toolbar_voice_cb, sess);

	Fl_Button *ban_btn = new Fl_Button (tbx + 90, content_y, 40, 24, "+b");
	ban_btn->tooltip (_("Ban"));
	ban_btn->callback (toolbar_ban_cb, sess);

	Fl_Button *kick_btn = new Fl_Button (tbx + 135, content_y, 40, 24, "K");
	kick_btn->tooltip (_("Kick"));
	kick_btn->callback (toolbar_kick_cb, sess);

	toolbar->end ();

	int text_w = content_w - 190;
	int text_h = content_h - 40;
	Fl_Text_Display *display = new ChatDisplay (content_x, content_y + 26, text_w, text_h);
	display->wrap_mode (Fl_Text_Display::WRAP_AT_BOUNDS, 0);
	Fl_Text_Buffer *buffer = new Fl_Text_Buffer ();
	display->buffer (buffer);
	Fl_Text_Buffer *stylebuf = new Fl_Text_Buffer ();

	UserBrowser *users = new UserBrowser (content_x + text_w + 10, content_y + 26, 170, text_h, sess);
	users->has_scrollbar (Fl_Browser::VERTICAL);

	grp->end ();
	content_stack->add (grp);
	grp->hide ();

	SessionUI ui;
	ui.tab = grp;
	ui.display = display;
	ui.buffer = buffer;
	ui.style_buffer = stylebuf;
	ui.topic = topic;
	ui.topic_btn = topic_btn;
	ui.user_browser = users;
	ui.toolbar = toolbar;
	ui.op_btn = op_btn;
	ui.voice_btn = voice_btn;
	ui.ban_btn = ban_btn;
	ui.kick_btn = kick_btn;
	auto inserted = session_ui_map.emplace (sess, std::move (ui));
	if (users)
		users->set_owner (&inserted.first->second);

	// Apply font settings to new widgets
	std::string fname;
	int fsize;
	parse_font_spec (prefs.hex_text_font_main, fname, fsize);
	apply_font_to_widgets (fname, fsize);
	if (inserted.first->second.display)
	{
		inserted.first->second.display->textfont (FL_COURIER);
		inserted.first->second.display->textsize (fsize);
		static Fl_Text_Display::Style_Table_Entry style_table[58];
		build_style_table (style_table, fsize);
		if (inserted.first->second.style_buffer)
		{
			inserted.first->second.display->highlight_data (
				inserted.first->second.style_buffer,
				style_table, 58, 'A', nullptr, nullptr);
		}
	}
	if (inserted.first->second.user_browser)
	{
		inserted.first->second.user_browser->textfont (FL_COURIER);
		inserted.first->second.user_browser->textsize (fsize);
	}

	session_tree_rebuild ();

	if (!current_tab)
		show_session_content (sess);

	return &inserted.first->second;
}

static void
append_text (session *sess, const char *text)
{
	SessionUI *ui = ensure_session_ui (sess ? sess : current_tab);
	if (!ui || !ui->buffer)
		return;

	const char *msg = text ? text : "";

	std::string out;
	std::string styles;

	// Timestamp
	if (prefs.hex_stamp_text)
	{
		char tbuf[64];
		time_t now = time (NULL);
		struct tm *tm = localtime (&now);
		const char *fmt = prefs.hex_stamp_text_format[0] ? prefs.hex_stamp_text_format : "%H:%M:%S";
		if (tm && strftime (tbuf, sizeof tbuf, fmt, tm))
		{
			out += tbuf;
			out += " ";
			styles.append (strlen (tbuf) + 1, 'A');
		}
	}

	// Parse CTCP/action markup
	auto append_char = [&](char c, char style) {
		out.push_back (c);
		styles.push_back (style);
	};

	auto hash_nick_color = [](const std::string &nick) -> int {
		int sum = 0;
		for (unsigned char ch : nick)
			sum += ch;
		return (sum % 16);
	};

	// Detect nick for simple coloring (<nick> or "* nick")
	int nick_start = -1;
	int nick_len = 0;
	int nick_color = -1;
	{
		const char *p = msg;
		int offset = 0;
		while (*p == ' ' || *p == '\t') { p++; offset++; }
		if (*p == '<')
		{
			p++; offset++;
			const char *nick_begin = p;
			while (*p && *p != '>' && *p != ' ')
				p++;
			if (*p == '>')
			{
				nick_start = offset;
				nick_len = (int)(p - nick_begin);
				nick_color = hash_nick_color (std::string (nick_begin, nick_len));
			}
		}
		else if (p[0] == '*' && p[1] == ' ')
		{
			offset += 2; p += 2;
			const char *nick_begin = p;
			while (*p && *p != ' ')
				p++;
			if (p > nick_begin)
			{
				nick_start = offset;
				nick_len = (int)(p - nick_begin);
				nick_color = hash_nick_color (std::string (nick_begin, nick_len));
			}
		}
	}

	int out_pos = (int)out.size ();

	if (msg[0] == '\001' && strncmp (msg, "\001ACTION ", 8) == 0)
	{
		const char *body = msg + 8;
		size_t len = strlen (body);
		if (len > 0 && body[len - 1] == '\001')
			len--;
		out.append ("* ");
		styles.append (2, 'B');
		for (size_t i = 0; i < len; i++)
			append_char (body[i], 'B');
		out_pos += 2;
	}
	else
	{
		bool in_ctcp = false;
		int fg = -1;
		bool bold = false;
		bool underline = false;
		auto style_for_state = [&](bool hyperlink) -> char {
			if (hyperlink)
				return static_cast<char>('A' + 57);
			if (in_ctcp && fg < 0 && !bold && !underline)
				return 'C'; // CTCP bold/blue style
			int base = (fg >= 0 && fg < 16) ? 3 + fg : 0;
			int region = 19;
			if (bold)
				base += region;
			else if (underline)
				base += region * 2;
			return static_cast<char>('A' + base);
		};

		for (const char *p = msg; *p; )
		{
			unsigned char ch = (unsigned char)*p;
			if (ch == '\001')
			{
				in_ctcp = !in_ctcp;
				p++;
				continue;
			}
			// Strip common IRC formatting codes (color/bold/underline/reset/bell) to avoid control boxes
			if (ch == 0x03) /* mIRC color */
			{
				p++;
				// optional fg[,bg]
				for (int k = 0; k < 2 && g_ascii_isdigit (*p); k++, p++) {}
				if (*p == ',')
				{
					p++;
					for (int k = 0; k < 2 && g_ascii_isdigit (*p); k++, p++) {}
				}
				continue;
			}
			if (ch == 0x02) { bold = !bold; p++; continue; } // bold toggle
			if (ch == 0x1f) { underline = !underline; p++; continue; } // underline toggle
			if (ch == 0x16) { p++; continue; } // reverse not handled yet
			if (ch == 0x0f) { fg = -1; bold = false; underline = false; p++; continue; }
			if (ch == 0x07) { p++; continue; } // bell

			// URL detection
			if (looks_like_url ((const char *)p))
			{
				const char *start = p;
				while (*p && !g_ascii_isspace (*p) && (unsigned char)*p >= 0x20)
					p++;
				for (const char *q = start; q < p; q++)
				{
					append_char (*q, style_for_state (true));
					out_pos++;
				}
				continue;
			}

			// Nick coloring
			bool apply_nick_color = (nick_start >= 0 && nick_color >= 0 &&
				out_pos >= nick_start && out_pos < nick_start + nick_len);
			int saved_fg = fg;
			if (apply_nick_color)
				fg = nick_color;

			append_char (*p, style_for_state (false));
			out_pos++;
			fg = saved_fg;
			p++;
		}
	}

	if (out.empty () || out.back () != '\n')
	{
		out.push_back ('\n');
		styles.push_back ('A');
	}

	ui->buffer->append (out.c_str ());
	if (ui->style_buffer && ui->style_buffer->length () <= ui->buffer->length ())
		ui->style_buffer->append (styles.c_str ());

	int len = ui->buffer->length ();
	if (ui->display)
	{
		ui->display->insert_position (len);
		ui->display->show_insert_position ();
	}
	if (sess && sess != current_tab && ui->tab)
	{
		ui->tab->labelcolor (FL_DARK_BLUE);
		ui->tab->redraw_label ();
	}
}

static void
update_tab_title (session *sess)
{
	if (!sess || !main_win)
		return;
	const char *label = sess->channel[0] ? sess->channel : _("server");
	main_win->label (label);
}

static void
update_user_browser (SessionUI *ui)
{
	if (!ui || !ui->user_browser)
		return;
	ui->user_browser->clear ();
	int row = 1;
	for (auto &entry : ui->users)
	{
		std::string label = entry.second;
		// FLTK treats leading '@' as a label control code; escape ops so they render
		if (!label.empty () && label[0] == '@')
			label.insert (0, "@");
		ui->user_browser->add (label.c_str ());
		row++;
	}
	ui->userlist_dirty = false;
}

static gboolean
userlist_idle_cb (void *)
{
	for (auto &pair : session_ui_map)
	{
		if (pair.second.userlist_dirty)
			update_user_browser (&pair.second);
	}
	userlist_idle_scheduled = false;
	return FALSE;
}

static void
schedule_userlist_refresh (void)
{
	if (!userlist_idle_scheduled)
	{
		g_idle_add (userlist_idle_cb, nullptr);
		userlist_idle_scheduled = true;
	}
}

static void tab_changed_cb (Fl_Widget *, void *) {}

// ============================================================
// Menu system callbacks
// ============================================================

static void
menu_server_connect_cb (Fl_Widget *, void *)
{
	if (current_sess)
		fe_serverlist_open (current_sess);
}

static void
menu_server_disconnect_cb (Fl_Widget *, void *)
{
	if (current_sess && current_sess->server)
		handle_command (current_sess, (char *)"DISCON", FALSE);
}

static void
menu_server_reconnect_cb (Fl_Widget *, void *)
{
	if (current_sess && current_sess->server)
		handle_command (current_sess, (char *)"RECONNECT", FALSE);
}

// Forward declaration for join dialog
static void joind_open (server *serv);

static void
menu_join_channel_cb (Fl_Widget *, void *)
{
	if (current_sess && current_sess->server)
		joind_open (current_sess->server);
}

static void
menu_part_channel_cb (Fl_Widget *, void *)
{
	if (current_sess && current_sess->channel[0])
		handle_command (current_sess, (char *)"PART", FALSE);
}

static void
menu_quit_cb (Fl_Widget *, void *)
{
	handle_command (current_sess, (char *)"QUIT", FALSE);
	fe_exit ();
}

static void
menu_clear_cb (Fl_Widget *, void *)
{
	if (current_sess)
		fe_text_clear (current_sess, 0);
}

static void
menu_search_cb (Fl_Widget *, void *)
{
	const char *term = fl_input (_("Search for:"), "");
	if (term && *term && current_sess)
	{
		char buf[512];
		g_snprintf (buf, sizeof buf, "LASTLOG %s", term);
		handle_command (current_sess, buf, FALSE);
	}
}

static void
menu_save_text_cb (Fl_Widget *, void *)
{
	const char *filename = fl_file_chooser (_("Save text buffer"), "*.txt", nullptr);
	if (filename && current_sess)
	{
		SessionUI *ui = session_ui_map.count (current_sess) ? &session_ui_map[current_sess] : nullptr;
		if (ui && ui->buffer)
		{
			FILE *f = fopen (filename, "w");
			if (f)
			{
				char *text = ui->buffer->text ();
				if (text)
				{
					fputs (text, f);
					free (text);
				}
				fclose (f);
			}
		}
	}
}

static void
menu_chanlist_cb (Fl_Widget *, void *)
{
	if (current_sess && current_sess->server)
		fe_open_chan_list (current_sess->server, nullptr, TRUE);
}

static void
menu_rawlog_cb (Fl_Widget *, void *)
{
	if (current_sess && current_sess->server)
		rawlog_open (current_sess->server);
}

static void
menu_url_grabber_cb (Fl_Widget *, void *)
{
	url_grabber_open ();
}

static void
menu_dcc_recv_cb (Fl_Widget *, void *)
{
	fe_dcc_open_recv_win (FALSE);
}

static void
menu_dcc_chat_cb (Fl_Widget *, void *)
{
	fe_dcc_open_chat_win (FALSE);
}

static void
menu_prefs_cb (Fl_Widget *, void *)
{
	prefs_open ();
}

static void
menu_about_cb (Fl_Widget *, void *)
{
	fl_message (_("HexChat (FLTK Frontend)\n\nVersion %s\n\nAn IRC client with FLTK GUI."), PACKAGE_VERSION);
}

static void
menu_notify_list_cb (Fl_Widget *, void *)
{
	notify_open ();
}

static void
menu_ignore_list_cb (Fl_Widget *, void *)
{
	ignore_open ();
}

static void
menu_ban_list_cb (Fl_Widget *, void *)
{
	if (current_sess && current_sess->channel[0])
		banlist_open (current_sess);
}

static void
menu_away_cb (Fl_Widget *, void *)
{
	const char *reason = fl_input (_("Away reason:"), _("Away"));
	if (reason && current_sess)
	{
		char buf[512];
		g_snprintf (buf, sizeof buf, "AWAY %s", reason);
		handle_command (current_sess, buf, FALSE);
	}
}

static void
menu_back_cb (Fl_Widget *, void *)
{
	if (current_sess)
		handle_command (current_sess, (char *)"BACK", FALSE);
}

static void
menu_nick_cb (Fl_Widget *, void *)
{
	const char *newnick = fl_input (_("New nickname:"), current_sess && current_sess->server ? current_sess->server->nick : "");
	if (newnick && *newnick && current_sess)
	{
		char buf[512];
		g_snprintf (buf, sizeof buf, "NICK %s", newnick);
		handle_command (current_sess, buf, FALSE);
	}
}

static void
menu_invisible_cb (Fl_Widget *w, void *)
{
	if (current_sess)
	{
		Fl_Menu_Item *mi = (Fl_Menu_Item *)w;
		if (mi && mi->value ())
			handle_command (current_sess, (char *)"MODE +i", FALSE);
		else
			handle_command (current_sess, (char *)"MODE -i", FALSE);
	}
}

static void
menu_receive_notices_cb (Fl_Widget *w, void *)
{
	if (current_sess)
	{
		Fl_Menu_Item *mi = (Fl_Menu_Item *)w;
		if (mi && mi->value ())
			handle_command (current_sess, (char *)"MODE +s", FALSE);
		else
			handle_command (current_sess, (char *)"MODE -s", FALSE);
	}
}

static void
menu_receive_wallops_cb (Fl_Widget *w, void *)
{
	if (current_sess)
	{
		Fl_Menu_Item *mi = (Fl_Menu_Item *)w;
		if (mi && mi->value ())
			handle_command (current_sess, (char *)"MODE +w", FALSE);
		else
			handle_command (current_sess, (char *)"MODE -w", FALSE);
	}
}

// ============================================================
// DCC Window Functions
// ============================================================

static void
dcc_window_close_cb (Fl_Widget *w, void *data)
{
	DCCWindow *dw = static_cast<DCCWindow *>(data);
	if (dw && dw->window)
	{
		dw->window->hide ();
		delete dw->window;
		dw->window = nullptr;
		dw->list = nullptr;
	}
}

static void
dcc_abort_cb (Fl_Widget *, void *data)
{
	DCCWindow *dw = static_cast<DCCWindow *>(data);
	if (!dw || !dw->list)
		return;
	int sel = dw->list->value ();
	if (sel <= 0)
		return;
	void *ptr = dw->list->data (sel);
	if (ptr && current_sess)
	{
		DCC *dcc = static_cast<DCC *>(ptr);
		dcc_abort (current_sess, dcc);
	}
}

static void
dcc_accept_cb (Fl_Widget *, void *data)
{
	DCCWindow *dw = static_cast<DCCWindow *>(data);
	if (!dw || !dw->list)
		return;
	int sel = dw->list->value ();
	if (sel <= 0)
		return;
	void *ptr = dw->list->data (sel);
	if (ptr)
	{
		DCC *dcc = static_cast<DCC *>(ptr);
		if (dcc->type == TYPE_RECV || dcc->type == TYPE_CHATRECV)
			dcc_get (dcc);
	}
}

static void
dcc_resume_cb (Fl_Widget *, void *data)
{
	DCCWindow *dw = static_cast<DCCWindow *>(data);
	if (!dw || !dw->list)
		return;
	int sel = dw->list->value ();
	if (sel <= 0)
		return;
	void *ptr = dw->list->data (sel);
	if (ptr)
	{
		DCC *dcc = static_cast<DCC *>(ptr);
		if (dcc->type == TYPE_RECV)
			dcc_resume (dcc);
	}
}

static const char *
dcc_status_name (int stat)
{
	switch (stat)
	{
	case STAT_QUEUED:     return _("Queued");
	case STAT_ACTIVE:     return _("Active");
	case STAT_FAILED:     return _("Failed");
	case STAT_DONE:       return _("Done");
	case STAT_CONNECTING: return _("Connecting");
	case STAT_ABORTED:    return _("Aborted");
	default:              return _("Unknown");
	}
}

static void
dcc_fill_list (DCCWindow *dw, bool is_chat)
{
	if (!dw || !dw->list)
		return;
	dw->list->clear ();

	GSList *list = dcc_list;
	while (list)
	{
		DCC *dcc = static_cast<DCC *>(list->data);
		bool show = false;

		if (is_chat)
		{
			if (dcc->type == TYPE_CHATSEND || dcc->type == TYPE_CHATRECV)
				show = true;
		}
		else
		{
			if (dcc->type == TYPE_SEND && (dw->view_mode & 2))
				show = true;
			if (dcc->type == TYPE_RECV && (dw->view_mode & 1))
				show = true;
		}

		if (show)
		{
			char buf[512];
			if (is_chat)
			{
				g_snprintf (buf, sizeof buf, "%s\t%s\t%s",
					dcc_status_name (dcc->dccstat),
					dcc->nick ? dcc->nick : "",
					ctime (&dcc->starttime));
			}
			else
			{
				float perc = dcc->size ? ((float)dcc->pos * 100.0f / (float)dcc->size) : 0.0f;
				float speed = dcc->cps / 1024.0f;
				g_snprintf (buf, sizeof buf, "%s\t%s\t%s\t%.0f%%\t%.1f KB/s\t%s",
					dcc->type == TYPE_SEND ? "UP" : "DN",
					dcc_status_name (dcc->dccstat),
					dcc->file ? file_part (dcc->file) : "",
					perc,
					speed,
					dcc->nick ? dcc->nick : "");
			}
			dw->list->add (buf, dcc);
		}
		list = list->next;
	}
}

static void
dcc_open_file_window (int passive)
{
	if (dcc_file_window.window)
	{
		if (!passive)
			dcc_file_window.window->show ();
		dcc_fill_list (&dcc_file_window, false);
		return;
	}

	Fl_Window *win = new Fl_Window (600, 350, _("Uploads and Downloads - HexChat"));
	dcc_file_window.window = win;

	dcc_file_window.list = new Fl_Select_Browser (10, 10, 580, 250);
	static int dcc_widths[] = {30, 80, 200, 60, 80, 100, 0};
	dcc_file_window.list->column_widths (dcc_widths);

	int bx = 10, by = 270;
	dcc_file_window.abort_btn = new Fl_Button (bx, by, 90, 25, _("Abort"));
	dcc_file_window.abort_btn->callback (dcc_abort_cb, &dcc_file_window);
	bx += 100;

	dcc_file_window.accept_btn = new Fl_Button (bx, by, 90, 25, _("Accept"));
	dcc_file_window.accept_btn->callback (dcc_accept_cb, &dcc_file_window);
	bx += 100;

	dcc_file_window.resume_btn = new Fl_Button (bx, by, 90, 25, _("Resume"));
	dcc_file_window.resume_btn->callback (dcc_resume_cb, &dcc_file_window);

	dcc_file_window.file_label = new Fl_Box (10, 305, 580, 20, "");
	dcc_file_window.file_label->align (FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
	dcc_file_window.address_label = new Fl_Box (10, 325, 580, 20, "");
	dcc_file_window.address_label->align (FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	win->callback (dcc_window_close_cb, &dcc_file_window);
	win->end ();
	win->show ();

	dcc_fill_list (&dcc_file_window, false);
}

static void
dcc_open_chat_window (int passive)
{
	if (dcc_chat_window.window)
	{
		if (!passive)
			dcc_chat_window.window->show ();
		dcc_fill_list (&dcc_chat_window, true);
		return;
	}

	Fl_Window *win = new Fl_Window (500, 250, _("DCC Chat List - HexChat"));
	dcc_chat_window.window = win;

	dcc_chat_window.list = new Fl_Select_Browser (10, 10, 480, 180);

	int bx = 10, by = 200;
	dcc_chat_window.abort_btn = new Fl_Button (bx, by, 90, 25, _("Abort"));
	dcc_chat_window.abort_btn->callback (dcc_abort_cb, &dcc_chat_window);
	bx += 100;

	dcc_chat_window.accept_btn = new Fl_Button (bx, by, 90, 25, _("Accept"));
	dcc_chat_window.accept_btn->callback (dcc_accept_cb, &dcc_chat_window);

	win->callback (dcc_window_close_cb, &dcc_chat_window);
	win->end ();
	win->show ();

	dcc_fill_list (&dcc_chat_window, true);
}

// ============================================================
// Server List Window Functions
// ============================================================

static void
servlist_update_network_details (void)
{
	if (!servlist_window.window)
		return;

	ircnet *net = servlist_window.selected_net;

	// Clear fav channels
	if (servlist_window.fav_channels)
		servlist_window.fav_channels->clear ();
	if (servlist_window.server_list)
		servlist_window.server_list->clear ();

	if (!net)
		return;

	// Populate servers
	GSList *slist = net->servlist;
	while (slist)
	{
		ircserver *serv = static_cast<ircserver *>(slist->data);
		if (serv && serv->hostname && servlist_window.server_list)
			servlist_window.server_list->add (serv->hostname, serv);
		slist = slist->next;
	}

	// Populate favorite channels
	GSList *flist = net->favchanlist;
	while (flist)
	{
		favchannel *fav = static_cast<favchannel *>(flist->data);
		if (fav && fav->name)
			servlist_window.fav_channels->add (fav->name);
		flist = flist->next;
	}

	// Update network-specific settings
	if (servlist_window.auto_connect)
		servlist_window.auto_connect->value ((net->flags & FLAG_AUTO_CONNECT) ? 1 : 0);
	if (servlist_window.use_ssl)
		servlist_window.use_ssl->value ((net->flags & FLAG_USE_SSL) ? 1 : 0);
	if (servlist_window.use_global)
		servlist_window.use_global->value ((net->flags & FLAG_USE_GLOBAL) ? 1 : 0);
	if (servlist_window.cycle_servers)
		servlist_window.cycle_servers->value ((net->flags & FLAG_CYCLE) ? 1 : 0);
	if (servlist_window.allow_invalid)
		servlist_window.allow_invalid->value ((net->flags & FLAG_ALLOW_INVALID) ? 1 : 0);
	if (servlist_window.use_proxy)
		servlist_window.use_proxy->value ((net->flags & FLAG_USE_PROXY) ? 1 : 0);

	// Update nick/user/real if not using global
	if (!(net->flags & FLAG_USE_GLOBAL))
	{
		if (servlist_window.nick1_input && net->nick)
			servlist_window.nick1_input->value (net->nick);
		if (servlist_window.nick2_input && net->nick2)
			servlist_window.nick2_input->value (net->nick2);
		if (servlist_window.username_input && net->user)
			servlist_window.username_input->value (net->user);
		if (servlist_window.realname_input && net->real)
			servlist_window.realname_input->value (net->real);
	}
	else
	{
		if (servlist_window.nick1_input)
			servlist_window.nick1_input->value (prefs.hex_irc_nick1);
		if (servlist_window.nick2_input)
			servlist_window.nick2_input->value (prefs.hex_irc_nick2);
		if (servlist_window.username_input)
			servlist_window.username_input->value (prefs.hex_irc_user_name);
		if (servlist_window.realname_input)
			servlist_window.realname_input->value (prefs.hex_irc_real_name);
	}

	if (servlist_window.password_input)
		servlist_window.password_input->value (net->pass ? net->pass : "");
	if (servlist_window.login_type)
		servlist_window.login_type->value (net->logintype);
	if (servlist_window.sasl_user)
		servlist_window.sasl_user->value (net->nick ? net->nick : "");
	if (servlist_window.sasl_pass)
		servlist_window.sasl_pass->value (net->pass ? net->pass : "");
	if (servlist_window.encoding_input)
		servlist_window.encoding_input->value (net->encoding ? net->encoding : "UTF-8");
}


static void
servlist_network_select_cb (Fl_Widget *, void *)
{
	if (!servlist_window.network_list)
		return;

	int sel = servlist_window.network_list->value ();
	if (sel <= 0)
	{
		servlist_window.selected_net = nullptr;
		servlist_update_network_details ();
		return;
	}

	void *data = servlist_window.network_list->data (sel);
	servlist_window.selected_net = static_cast<ircnet *>(data);
	servlist_update_network_details ();
}

static void
servlist_connect_cb (Fl_Widget *, void *)
{
	if (!servlist_window.selected_net)
	{
		fl_alert (_("Please select a network first."));
		return;
	}

	// Save any changes
	ircnet *net = servlist_window.selected_net;

	// Update flags
	net->flags = 0;
	if (servlist_window.auto_connect && servlist_window.auto_connect->value ())
		net->flags |= FLAG_AUTO_CONNECT;
	if (servlist_window.use_ssl && servlist_window.use_ssl->value ())
		net->flags |= FLAG_USE_SSL;
	if (servlist_window.use_global && servlist_window.use_global->value ())
		net->flags |= FLAG_USE_GLOBAL;
	if (servlist_window.cycle_servers && servlist_window.cycle_servers->value ())
		net->flags |= FLAG_CYCLE;
	if (servlist_window.allow_invalid && servlist_window.allow_invalid->value ())
		net->flags |= FLAG_ALLOW_INVALID;
	if (servlist_window.use_proxy && servlist_window.use_proxy->value ())
		net->flags |= FLAG_USE_PROXY;

	// Credentials and identity
	if (servlist_window.password_input)
	{
		g_free (net->pass);
		net->pass = g_strdup (servlist_window.password_input->value ());
	}
	if (servlist_window.login_type)
		net->logintype = servlist_window.login_type->value ();
	if (servlist_window.encoding_input)
	{
		g_free (net->encoding);
		net->encoding = g_strdup (servlist_window.encoding_input->value ());
	}
	if (!(net->flags & FLAG_USE_GLOBAL))
	{
		if (servlist_window.nick1_input)
		{
			g_free (net->nick);
			net->nick = g_strdup (servlist_window.nick1_input->value ());
		}
		if (servlist_window.nick2_input)
		{
			g_free (net->nick2);
			net->nick2 = g_strdup (servlist_window.nick2_input->value ());
		}
		if (servlist_window.username_input)
		{
			g_free (net->user);
			net->user = g_strdup (servlist_window.username_input->value ());
		}
		if (servlist_window.realname_input)
		{
			g_free (net->real);
			net->real = g_strdup (servlist_window.realname_input->value ());
		}
	}

	if (servlist_window.sasl_user)
	{
		g_free (net->nick);
		net->nick = g_strdup (servlist_window.sasl_user->value ());
	}
	if (servlist_window.sasl_pass)
	{
		g_free (net->pass);
		net->pass = g_strdup (servlist_window.sasl_pass->value ());
	}

	servlist_save ();

	// Connect
	servlist_connect (servlist_window.sess, net, TRUE);

	// Close window
	if (servlist_window.window)
		servlist_window.window->hide ();
}

static void
servlist_add_network_cb (Fl_Widget *, void *)
{
	const char *name = fl_input (_("Network name:"), _("New Network"));
	if (!name || !*name)
		return;

	ircnet *net = servlist_net_add ((char *)name, nullptr, FALSE);
	if (net)
	{
		if (servlist_window.network_list)
		{
			servlist_window.network_list->add (name, net);
			servlist_window.network_list->value (servlist_window.network_list->size ());
		}
		servlist_window.selected_net = net;
		servlist_update_network_details ();
		servlist_save ();
	}
}

static void
servlist_remove_network_cb (Fl_Widget *, void *)
{
	if (!servlist_window.selected_net)
		return;

	int choice = fl_choice (_("Remove network '%s'?"), _("Cancel"), _("Remove"), nullptr,
		servlist_window.selected_net->name);
	if (choice != 1)
		return;

	servlist_net_remove (servlist_window.selected_net);
	servlist_window.selected_net = nullptr;
	if (servlist_window.network_list)
	{
		int sel = servlist_window.network_list->value ();
		if (sel > 0)
			servlist_window.network_list->remove (sel);
	}
	servlist_update_network_details ();
	servlist_save ();
}

static void
servlist_add_server_cb (Fl_Widget *, void *)
{
	if (!servlist_window.selected_net)
	{
		fl_alert (_("Please select a network first."));
		return;
	}

	const char *host = fl_input (_("Server hostname:"), "irc.example.org");
	if (!host || !*host)
		return;

	servlist_server_add (servlist_window.selected_net, (char *)host);
	if (servlist_window.server_list)
		servlist_window.server_list->add (host);
	servlist_save ();
}

static void
servlist_remove_server_cb (Fl_Widget *, void *)
{
	if (!servlist_window.selected_net || !servlist_window.server_list)
		return;

	int sel = servlist_window.server_list->value ();
	if (sel <= 0)
		return;

	const char *hostname = servlist_window.server_list->text (sel);
	if (!hostname)
		return;

	ircserver *serv = servlist_server_find (servlist_window.selected_net, (char *)hostname, nullptr);
	if (serv)
	{
		servlist_server_remove (servlist_window.selected_net, serv);
		servlist_window.server_list->remove (sel);
		servlist_save ();
	}
}

static void
servlist_add_channel_cb (Fl_Widget *, void *)
{
	if (!servlist_window.selected_net)
	{
		fl_alert (_("Please select a network first."));
		return;
	}

	const char *chan = fl_input (_("Channel name:"), "#");
	if (!chan || !*chan)
		return;

	const char *key = servlist_window.fav_key_input ? servlist_window.fav_key_input->value () : "";
	char buf[512];
	if (key && *key)
		g_snprintf (buf, sizeof buf, "%s,%s", chan, key);
	else
		g_snprintf (buf, sizeof buf, "%s", chan);

	servlist_favchan_add (servlist_window.selected_net, buf);
	if (key && *key)
		servlist_window.fav_channels->add ((std::string(chan) + "\t" + key).c_str ());
	else
		servlist_window.fav_channels->add (chan);
	servlist_save ();
}

static void
servlist_remove_channel_cb (Fl_Widget *, void *)
{
	if (!servlist_window.selected_net || !servlist_window.fav_channels)
		return;

	int sel = servlist_window.fav_channels->value ();
	if (sel <= 0)
		return;

	const char *channame = servlist_window.fav_channels->text (sel);
	if (!channame)
		return;

	char namebuf[256];
	snprintf (namebuf, sizeof namebuf, "%s", channame);
	char *tab = strchr (namebuf, '\t');
	if (tab) *tab = 0;

	favchannel *fav = servlist_favchan_find (servlist_window.selected_net, namebuf, nullptr);
	if (fav)
	{
		servlist_favchan_remove (servlist_window.selected_net, fav);
		servlist_window.fav_channels->remove (sel);
		servlist_save ();
	}
}

static void
servlist_window_close_cb (Fl_Widget *, void *)
{
	if (servlist_window.window)
	{
		servlist_window.window->hide ();
		delete servlist_window.window;
		servlist_window.window = nullptr;
	}
}

static void
servlist_open (session *sess)
{
	if (servlist_window.window)
	{
		servlist_window.window->show ();
		return;
	}

	servlist_window.sess = sess;

	Fl_Window *win = new Fl_Window (800, 550, _("FlexChat: Network List"));
	servlist_window.window = win;

	// Left side: Network list
	new Fl_Box (10, 10, 200, 20, _("Networks"));
	servlist_window.network_list = new Fl_Hold_Browser (10, 35, 200, 300);
	servlist_window.network_list->callback (servlist_network_select_cb);

	Fl_Button *add_net_btn = new Fl_Button (10, 340, 95, 25, _("Add"));
	add_net_btn->callback (servlist_add_network_cb);
	Fl_Button *rem_net_btn = new Fl_Button (115, 340, 95, 25, _("Remove"));
	rem_net_btn->callback (servlist_remove_network_cb);

	// Middle: Server list and favorites
	new Fl_Box (220, 10, 200, 20, _("Servers"));
	servlist_window.server_list = new Fl_Hold_Browser (220, 35, 200, 150);

	Fl_Button *add_srv_btn = new Fl_Button (220, 190, 95, 25, _("Add"));
	add_srv_btn->callback (servlist_add_server_cb);
	Fl_Button *rem_srv_btn = new Fl_Button (325, 190, 95, 25, _("Remove"));
	rem_srv_btn->callback (servlist_remove_server_cb);

	new Fl_Box (220, 225, 200, 20, _("Favorite Channels"));
	servlist_window.fav_channels = new Fl_Hold_Browser (220, 250, 200, 85);

	Fl_Button *add_chan_btn = new Fl_Button (220, 340, 95, 25, _("Add"));
	add_chan_btn->callback (servlist_add_channel_cb);
	Fl_Button *rem_chan_btn = new Fl_Button (325, 340, 95, 25, _("Remove"));
	rem_chan_btn->callback (servlist_remove_channel_cb);

	new Fl_Box (220, 375, 200, 20, _("Channel Key:"));
	servlist_window.fav_key_input = new Fl_Input (220, 400, 200, 25);
	servlist_window.fav_key_input->tooltip (_("Key used when adding a favorite"));

	// Right side: User/settings
	int rx = 440, ry = 10;
	new Fl_Box (rx, ry, 100, 20, _("Your Details"));
	ry += 25;

	new Fl_Box (rx, ry, 60, 25, _("Nick 1:"));
	servlist_window.nick1_input = new Fl_Input (rx + 70, ry, 150, 25);
	servlist_window.nick1_input->value (prefs.hex_irc_nick1);
	ry += 30;

	new Fl_Box (rx, ry, 60, 25, _("Nick 2:"));
	servlist_window.nick2_input = new Fl_Input (rx + 70, ry, 150, 25);
	servlist_window.nick2_input->value (prefs.hex_irc_nick2);
	ry += 30;

	new Fl_Box (rx, ry, 60, 25, _("User:"));
	servlist_window.username_input = new Fl_Input (rx + 70, ry, 150, 25);
	servlist_window.username_input->value (prefs.hex_irc_user_name);
	ry += 30;

	new Fl_Box (rx, ry, 60, 25, _("Real:"));
	servlist_window.realname_input = new Fl_Input (rx + 70, ry, 150, 25);
	servlist_window.realname_input->value (prefs.hex_irc_real_name);
	ry += 35;

	// Network options
	servlist_window.use_global = new Fl_Check_Button (rx, ry, 200, 25, _("Use global user info"));
	servlist_window.use_global->value (1);
	ry += 25;

	servlist_window.auto_connect = new Fl_Check_Button (rx, ry, 200, 25, _("Auto connect"));
	ry += 25;

	servlist_window.use_ssl = new Fl_Check_Button (rx, ry, 200, 25, _("Use SSL/TLS"));
	ry += 25;

	servlist_window.cycle_servers = new Fl_Check_Button (rx, ry, 200, 25, _("Cycle servers"));
	ry += 30;

	servlist_window.allow_invalid = new Fl_Check_Button (rx, ry, 250, 25, _("Allow invalid certs"));
	ry += 25;
	servlist_window.use_proxy = new Fl_Check_Button (rx, ry, 200, 25, _("Use proxy"));
	ry += 30;

	new Fl_Box (rx, ry, 60, 25, _("Password:"));
	servlist_window.password_input = new Fl_Input (rx + 70, ry, 150, 25);
	servlist_window.password_input->type (FL_SECRET_INPUT);
	ry += 30;

	new Fl_Box (rx, ry, 60, 25, _("Login:"));
	servlist_window.login_type = new Fl_Choice (rx + 70, ry, 150, 25);
	servlist_window.login_type->add (_("Default"));
	servlist_window.login_type->add (_("NickServ MSG"));
	servlist_window.login_type->add (_("NickServ"));
	servlist_window.login_type->add (_("Challenge Auth"));
	servlist_window.login_type->add (_("SASL PLAIN"));
	servlist_window.login_type->add (_("Server Pass"));
	servlist_window.login_type->add (_("SASL External"));
	servlist_window.login_type->value (0);
	ry += 30;

	new Fl_Box (rx, ry, 60, 25, _("SASL User:"));
	servlist_window.sasl_user = new Fl_Input (rx + 70, ry, 150, 25);
	ry += 30;
	new Fl_Box (rx, ry, 60, 25, _("SASL Pass:"));
	servlist_window.sasl_pass = new Fl_Input (rx + 70, ry, 150, 25);
	servlist_window.sasl_pass->type (FL_SECRET_INPUT);
	ry += 30;

	new Fl_Box (rx, ry, 60, 25, _("Encoding:"));
	servlist_window.encoding_input = new Fl_Input (rx + 70, ry, 150, 25);
	servlist_window.encoding_input->value ("UTF-8");

	// Bottom buttons
	servlist_window.connect_btn = new Fl_Return_Button (580, 510, 100, 30, _("Connect"));
	servlist_window.connect_btn->callback (servlist_connect_cb);

	Fl_Button *close_btn = new Fl_Button (690, 510, 100, 30, _("Close"));
	close_btn->callback (servlist_window_close_cb);

	// Populate network list
	servlist_window.selected_net = nullptr;
	servlist_window.sess = sess;
	GSList *list = network_list;
	while (list)
	{
		ircnet *net = static_cast<ircnet *>(list->data);
		if (net && net->name && servlist_window.network_list)
			servlist_window.network_list->add (net->name, net);
		list = list->next;
	}
	if (servlist_window.network_list && servlist_window.network_list->size () > 0)
	{
		servlist_window.network_list->value (1);
		servlist_network_select_cb (nullptr, nullptr);
	}

	win->callback (servlist_window_close_cb);
	win->end ();
	win->show ();
}

// ============================================================
// Preferences Window Functions
// ============================================================

static void
prefs_save_cb (Fl_Widget *, void *)
{
	if (!prefs_window.window)
		return;

	// Interface settings
	if (prefs_window.show_timestamps)
		prefs.hex_stamp_text = prefs_window.show_timestamps->value ();
	if (prefs_window.timestamp_format && *prefs_window.timestamp_format->value ())
		g_strlcpy (prefs.hex_stamp_text_format, prefs_window.timestamp_format->value (),
			sizeof (prefs.hex_stamp_text_format));
	if (prefs_window.colored_nicks)
		prefs.hex_text_color_nicks = prefs_window.colored_nicks->value ();

	// Spell checking settings
	if (prefs_window.enable_spell)
	{
		int old_spell = prefs.hex_gui_input_spell;
		prefs.hex_gui_input_spell = prefs_window.enable_spell->value ();
		if (prefs.hex_gui_input_spell && !old_spell && have_enchant)
			spell_init_broker ();
	}
	if (prefs_window.spell_langs)
	{
		const char *new_langs = prefs_window.spell_langs->value ();
		if (strcmp (new_langs, prefs.hex_text_spell_langs) != 0)
		{
			g_strlcpy (prefs.hex_text_spell_langs, new_langs, sizeof (prefs.hex_text_spell_langs));
			// Reinitialize spell checking with new languages
			if (have_enchant && prefs.hex_gui_input_spell)
			{
				spell_cleanup ();
				spell_init_broker ();
			}
		}
	}

	// Redraw input box to update spell checking display
	if (input_box)
		input_box->redraw ();

	// Chatting settings
	if (prefs_window.nick1 && *prefs_window.nick1->value ())
		g_strlcpy (prefs.hex_irc_nick1, prefs_window.nick1->value (), sizeof (prefs.hex_irc_nick1));
	if (prefs_window.nick2 && *prefs_window.nick2->value ())
		g_strlcpy (prefs.hex_irc_nick2, prefs_window.nick2->value (), sizeof (prefs.hex_irc_nick2));
	if (prefs_window.nick3 && *prefs_window.nick3->value ())
		g_strlcpy (prefs.hex_irc_nick3, prefs_window.nick3->value (), sizeof (prefs.hex_irc_nick3));
	if (prefs_window.username && *prefs_window.username->value ())
		g_strlcpy (prefs.hex_irc_user_name, prefs_window.username->value (), sizeof (prefs.hex_irc_user_name));
	if (prefs_window.realname && *prefs_window.realname->value ())
		g_strlcpy (prefs.hex_irc_real_name, prefs_window.realname->value (), sizeof (prefs.hex_irc_real_name));
	if (prefs_window.quit_msg)
		g_strlcpy (prefs.hex_irc_quit_reason, prefs_window.quit_msg->value (), sizeof (prefs.hex_irc_quit_reason));
	if (prefs_window.part_msg)
		g_strlcpy (prefs.hex_irc_part_reason, prefs_window.part_msg->value (), sizeof (prefs.hex_irc_part_reason));
	if (prefs_window.away_msg)
		g_strlcpy (prefs.hex_away_reason, prefs_window.away_msg->value (), sizeof (prefs.hex_away_reason));

	// Network settings
	if (prefs_window.auto_reconnect)
		prefs.hex_net_auto_reconnect = prefs_window.auto_reconnect->value ();
	if (prefs_window.reconnect_delay)
		prefs.hex_net_reconnect_delay = (int)prefs_window.reconnect_delay->value ();
	if (prefs_window.proxy_type)
		prefs.hex_net_proxy_type = prefs_window.proxy_type->value ();
	if (prefs_window.proxy_host)
		g_strlcpy (prefs.hex_net_proxy_host, prefs_window.proxy_host->value (), sizeof (prefs.hex_net_proxy_host));
	if (prefs_window.proxy_port)
		prefs.hex_net_proxy_port = (int)prefs_window.proxy_port->value ();

	// DCC settings
	if (prefs_window.dcc_dir)
		g_strlcpy (prefs.hex_dcc_dir, prefs_window.dcc_dir->value (), sizeof (prefs.hex_dcc_dir));
	if (prefs_window.dcc_completed_dir)
		g_strlcpy (prefs.hex_dcc_completed_dir, prefs_window.dcc_completed_dir->value (), sizeof (prefs.hex_dcc_completed_dir));
	if (prefs_window.dcc_port_first)
		prefs.hex_dcc_port_first = (int)prefs_window.dcc_port_first->value ();
	if (prefs_window.dcc_port_last)
		prefs.hex_dcc_port_last = (int)prefs_window.dcc_port_last->value ();

	// Logging settings
	if (prefs_window.enable_logging)
		prefs.hex_irc_logging = prefs_window.enable_logging->value ();
	if (prefs_window.log_dir && *prefs_window.log_dir->value ())
		g_strlcpy (prefs.hex_irc_logmask, prefs_window.log_dir->value (), sizeof (prefs.hex_irc_logmask));
	if (prefs_window.log_timestamp)
		g_strlcpy (prefs.hex_stamp_log_format, prefs_window.log_timestamp->value (), sizeof (prefs.hex_stamp_log_format));

	// Alert settings
	if (prefs_window.beep_on_msg)
		prefs.hex_input_beep_chans = prefs_window.beep_on_msg->value ();
	if (prefs_window.beep_on_hilight)
		prefs.hex_input_beep_hilight = prefs_window.beep_on_hilight->value ();
	if (prefs_window.beep_on_priv)
		prefs.hex_input_beep_priv = prefs_window.beep_on_priv->value ();
	if (prefs_window.flash_on_msg)
		prefs.hex_input_flash_chans = prefs_window.flash_on_msg->value ();
	if (prefs_window.flash_on_hilight)
		prefs.hex_input_flash_hilight = prefs_window.flash_on_hilight->value ();
	if (prefs_window.flash_on_priv)
		prefs.hex_input_flash_priv = prefs_window.flash_on_priv->value ();

	save_config ();
	fl_message (_("Preferences saved."));
}

static void
prefs_window_close_cb (Fl_Widget *, void *)
{
	if (prefs_window.window)
	{
		prefs_window.window->hide ();
		delete prefs_window.window;
		prefs_window.window = nullptr;
	}
}

static void
logging_dir_browse_cb (Fl_Widget *, void *)
{
	if (!prefs_window.log_dir)
		return;
	const char *cur = prefs_window.log_dir->value ();
	const char *picked = fl_dir_chooser (_("Select log directory"), cur && *cur ? cur : getenv ("HOME"), 1);
	if (picked)
		prefs_window.log_dir->value (picked);
}

static void
prefs_open (void)
{
	if (prefs_window.window)
	{
		prefs_window.window->show ();
		return;
	}

	Fl_Window *win = new Fl_Window (550, 450, _("HexChat: Preferences"));
	prefs_window.window = win;

	prefs_window.tabs = new Fl_Tabs (10, 10, 530, 380);

	// ===== Interface Tab =====
	Fl_Group *interface_grp = new Fl_Group (10, 35, 530, 355, _("Interface"));
	int y = 50;

	prefs_window.show_timestamps = new Fl_Check_Button (20, y, 200, 25, _("Show timestamps"));
	prefs_window.show_timestamps->value (prefs.hex_stamp_text);
	y += 30;

	new Fl_Box (20, y, 100, 25, _("Timestamp format:"));
	prefs_window.timestamp_format = new Fl_Input (130, y, 150, 25);
	prefs_window.timestamp_format->value (prefs.hex_stamp_text_format);
	y += 35;

	prefs_window.colored_nicks = new Fl_Check_Button (20, y, 200, 25, _("Colored nicknames"));
	prefs_window.colored_nicks->value (prefs.hex_text_color_nicks);
	y += 30;

	new Fl_Box (20, y, 100, 25, _("Font:"));
	prefs_window.font_input = new Fl_Input (130, y, 250, 25);
	prefs_window.font_input->value (prefs.hex_text_font_main);
	y += 35;

	prefs_window.enable_spell = new Fl_Check_Button (20, y, 200, 25, _("Enable spell checking"));
	prefs_window.enable_spell->value (prefs.hex_gui_input_spell);
	if (!have_enchant)
	{
		prefs_window.enable_spell->deactivate ();
		prefs_window.enable_spell->tooltip (_("Enchant library not found"));
	}
	y += 30;

	new Fl_Box (20, y, 100, 25, _("Spell languages:"));
	prefs_window.spell_langs = new Fl_Input (130, y, 250, 25);
	prefs_window.spell_langs->value (prefs.hex_text_spell_langs);
	prefs_window.spell_langs->tooltip (_("Comma-separated language codes (e.g., en,fr,de)"));
	if (!have_enchant)
		prefs_window.spell_langs->deactivate ();

	interface_grp->end ();

	// ===== Chatting Tab =====
	Fl_Group *chatting_grp = new Fl_Group (10, 35, 530, 355, _("Chatting"));
	y = 50;

	new Fl_Box (20, y, 80, 25, _("Nick 1:"));
	prefs_window.nick1 = new Fl_Input (110, y, 150, 25);
	prefs_window.nick1->value (prefs.hex_irc_nick1);
	y += 30;

	new Fl_Box (20, y, 80, 25, _("Nick 2:"));
	prefs_window.nick2 = new Fl_Input (110, y, 150, 25);
	prefs_window.nick2->value (prefs.hex_irc_nick2);
	y += 30;

	new Fl_Box (20, y, 80, 25, _("Nick 3:"));
	prefs_window.nick3 = new Fl_Input (110, y, 150, 25);
	prefs_window.nick3->value (prefs.hex_irc_nick3);
	y += 30;

	new Fl_Box (20, y, 80, 25, _("Username:"));
	prefs_window.username = new Fl_Input (110, y, 150, 25);
	prefs_window.username->value (prefs.hex_irc_user_name);
	y += 30;

	new Fl_Box (20, y, 80, 25, _("Real name:"));
	prefs_window.realname = new Fl_Input (110, y, 250, 25);
	prefs_window.realname->value (prefs.hex_irc_real_name);
	y += 35;

	new Fl_Box (20, y, 80, 25, _("Quit msg:"));
	prefs_window.quit_msg = new Fl_Input (110, y, 350, 25);
	prefs_window.quit_msg->value (prefs.hex_irc_quit_reason);
	y += 30;

	new Fl_Box (20, y, 80, 25, _("Part msg:"));
	prefs_window.part_msg = new Fl_Input (110, y, 350, 25);
	prefs_window.part_msg->value (prefs.hex_irc_part_reason);
	y += 30;

	new Fl_Box (20, y, 80, 25, _("Away msg:"));
	prefs_window.away_msg = new Fl_Input (110, y, 350, 25);
	prefs_window.away_msg->value (prefs.hex_away_reason);

	chatting_grp->end ();

	// ===== Network Tab =====
	Fl_Group *network_grp = new Fl_Group (10, 35, 530, 355, _("Network"));
	y = 50;

	prefs_window.auto_reconnect = new Fl_Check_Button (20, y, 200, 25, _("Auto reconnect"));
	prefs_window.auto_reconnect->value (prefs.hex_net_auto_reconnect);
	y += 30;

	new Fl_Box (20, y, 120, 25, _("Reconnect delay:"));
	prefs_window.reconnect_delay = new Fl_Spinner (150, y, 80, 25);
	prefs_window.reconnect_delay->minimum (1);
	prefs_window.reconnect_delay->maximum (600);
	prefs_window.reconnect_delay->value (prefs.hex_net_reconnect_delay);
	new Fl_Box (235, y, 50, 25, _("seconds"));
	y += 40;

	new Fl_Box (20, y, 100, 25, _("Proxy type:"));
	prefs_window.proxy_type = new Fl_Choice (130, y, 150, 25);
	prefs_window.proxy_type->add (_("Disabled"));
	prefs_window.proxy_type->add (_("Wingate"));
	prefs_window.proxy_type->add (_("SOCKS4"));
	prefs_window.proxy_type->add (_("SOCKS5"));
	prefs_window.proxy_type->add (_("HTTP"));
	prefs_window.proxy_type->value (prefs.hex_net_proxy_type);
	y += 30;

	new Fl_Box (20, y, 100, 25, _("Proxy host:"));
	prefs_window.proxy_host = new Fl_Input (130, y, 200, 25);
	prefs_window.proxy_host->value (prefs.hex_net_proxy_host);
	y += 30;

	new Fl_Box (20, y, 100, 25, _("Proxy port:"));
	prefs_window.proxy_port = new Fl_Spinner (130, y, 80, 25);
	prefs_window.proxy_port->minimum (1);
	prefs_window.proxy_port->maximum (65535);
	prefs_window.proxy_port->value (prefs.hex_net_proxy_port);

	network_grp->end ();

	// ===== DCC Tab =====
	Fl_Group *dcc_grp = new Fl_Group (10, 35, 530, 355, _("DCC"));
	y = 50;

	new Fl_Box (20, y, 120, 25, _("Download dir:"));
	prefs_window.dcc_dir = new Fl_Input (150, y, 300, 25);
	prefs_window.dcc_dir->value (prefs.hex_dcc_dir);
	y += 30;

	new Fl_Box (20, y, 120, 25, _("Completed dir:"));
	prefs_window.dcc_completed_dir = new Fl_Input (150, y, 300, 25);
	prefs_window.dcc_completed_dir->value (prefs.hex_dcc_completed_dir);
	y += 35;

	new Fl_Box (20, y, 120, 25, _("Port range:"));
	prefs_window.dcc_port_first = new Fl_Spinner (150, y, 80, 25);
	prefs_window.dcc_port_first->minimum (1024);
	prefs_window.dcc_port_first->maximum (65535);
	prefs_window.dcc_port_first->value (prefs.hex_dcc_port_first);
	new Fl_Box (235, y, 20, 25, _("-"));
	prefs_window.dcc_port_last = new Fl_Spinner (260, y, 80, 25);
	prefs_window.dcc_port_last->minimum (1024);
	prefs_window.dcc_port_last->maximum (65535);
	prefs_window.dcc_port_last->value (prefs.hex_dcc_port_last);

	dcc_grp->end ();

	// ===== Logging Tab =====
	Fl_Group *logging_grp = new Fl_Group (10, 35, 530, 355, _("Logging"));
	y = 50;

	prefs_window.enable_logging = new Fl_Check_Button (20, y, 200, 25, _("Enable logging"));
	prefs_window.enable_logging->value (prefs.hex_irc_logging);
	y += 35;

	new Fl_Box (20, y, 120, 25, _("Log directory:"));
	prefs_window.log_dir = new Fl_Input (150, y, 250, 25);
	prefs_window.log_dir->value (prefs.hex_irc_logmask);
	prefs_window.log_browse = new Fl_Button (410, y, 90, 25, _("Browse"));
	prefs_window.log_browse->callback (logging_dir_browse_cb);
	y += 35;

	new Fl_Box (20, y, 120, 25, _("Log timestamp:"));
	prefs_window.log_timestamp = new Fl_Input (150, y, 200, 25);
	prefs_window.log_timestamp->value (prefs.hex_stamp_log_format);

	logging_grp->end ();

	// ===== Alerts Tab =====
	Fl_Group *alerts_grp = new Fl_Group (10, 35, 530, 355, _("Alerts"));
	y = 50;

	new Fl_Box (20, y, 200, 25, _("Beep on:"));
	y += 25;

	prefs_window.beep_on_msg = new Fl_Check_Button (30, y, 180, 25, _("Channel messages"));
	prefs_window.beep_on_msg->value (prefs.hex_input_beep_chans);
	y += 25;

	prefs_window.beep_on_hilight = new Fl_Check_Button (30, y, 180, 25, _("Highlighted messages"));
	prefs_window.beep_on_hilight->value (prefs.hex_input_beep_hilight);
	y += 25;

	prefs_window.beep_on_priv = new Fl_Check_Button (30, y, 180, 25, _("Private messages"));
	prefs_window.beep_on_priv->value (prefs.hex_input_beep_priv);
	y += 35;

	new Fl_Box (20, y, 200, 25, _("Flash taskbar on:"));
	y += 25;

	prefs_window.flash_on_msg = new Fl_Check_Button (30, y, 180, 25, _("Channel messages"));
	prefs_window.flash_on_msg->value (prefs.hex_input_flash_chans);
	y += 25;

	prefs_window.flash_on_hilight = new Fl_Check_Button (30, y, 180, 25, _("Highlighted messages"));
	prefs_window.flash_on_hilight->value (prefs.hex_input_flash_hilight);
	y += 25;

	prefs_window.flash_on_priv = new Fl_Check_Button (30, y, 180, 25, _("Private messages"));
	prefs_window.flash_on_priv->value (prefs.hex_input_flash_priv);

	alerts_grp->end ();

	prefs_window.tabs->end ();

	// Bottom buttons
	Fl_Button *save_btn = new Fl_Button (330, 405, 100, 30, _("Save"));
	save_btn->callback (prefs_save_cb);

	Fl_Button *close_btn = new Fl_Button (440, 405, 100, 30, _("Close"));
	close_btn->callback (prefs_window_close_cb);

	win->callback (prefs_window_close_cb);
	win->end ();
	win->show ();
}

// ============================================================
// Raw Log Window Functions
// ============================================================

static void
rawlog_window_close_cb (Fl_Widget *, void *data)
{
	server *serv = static_cast<server *>(data);
	auto it = rawlog_windows.find (serv);
	if (it != rawlog_windows.end ())
	{
		if (it->second.window)
		{
			it->second.window->hide ();
			delete it->second.window;
		}
		rawlog_windows.erase (it);
	}
}

static void
rawlog_clear_cb (Fl_Widget *, void *data)
{
	server *serv = static_cast<server *>(data);
	auto it = rawlog_windows.find (serv);
	if (it != rawlog_windows.end () && it->second.buffer)
		it->second.buffer->text ("");
}

static void
rawlog_save_cb (Fl_Widget *, void *data)
{
	server *serv = static_cast<server *>(data);
	auto it = rawlog_windows.find (serv);
	if (it == rawlog_windows.end () || !it->second.buffer)
		return;

	const char *filename = fl_file_chooser (_("Save Raw Log"), "*.txt", nullptr);
	if (filename)
	{
		FILE *f = fopen (filename, "w");
		if (f)
		{
			char *text = it->second.buffer->text ();
			if (text)
			{
				fputs (text, f);
				free (text);
			}
			fclose (f);
		}
	}
}

static void
rawlog_open (server *serv)
{
	auto it = rawlog_windows.find (serv);
	if (it != rawlog_windows.end () && it->second.window)
	{
		it->second.window->show ();
		return;
	}

	RawLogWindow rlw;
	rlw.serv = serv;

	char title[256];
	const char *srv_name = (serv && serv->servername[0]) ? serv->servername : _("Server");
	g_snprintf (title, sizeof title, _("Raw Log (%s) - HexChat"),
		srv_name);

	Fl_Window *win = new Fl_Window (700, 500, nullptr);
	win->copy_label (title);
	rlw.window = win;

	rlw.display = new Fl_Text_Display (10, 10, 680, 420);
	rlw.display->wrap_mode (Fl_Text_Display::WRAP_AT_BOUNDS, 0);
	rlw.buffer = new Fl_Text_Buffer ();
	rlw.display->buffer (rlw.buffer);

	rlw.inbound = new Fl_Check_Button (10, 440, 100, 25, _("Inbound"));
	rlw.inbound->value (1);

	rlw.outbound = new Fl_Check_Button (120, 440, 100, 25, _("Outbound"));
	rlw.outbound->value (1);

	Fl_Button *clear_btn = new Fl_Button (480, 460, 100, 30, _("Clear"));
	clear_btn->callback (rawlog_clear_cb, serv);

	Fl_Button *save_btn = new Fl_Button (590, 460, 100, 30, _("Save"));
	save_btn->callback (rawlog_save_cb, serv);

	win->callback (rawlog_window_close_cb, serv);
	win->end ();
	win->show ();

	rawlog_windows[serv] = rlw;
}

static void
rawlog_append (server *serv, const char *text, int outbound)
{
	auto it = rawlog_windows.find (serv);
	if (it == rawlog_windows.end () || !it->second.buffer)
		return;

	// Check filters
	if (outbound && it->second.outbound && !it->second.outbound->value ())
		return;
	if (!outbound && it->second.inbound && !it->second.inbound->value ())
		return;

	char buf[2048];
	g_snprintf (buf, sizeof buf, "%s %s\n", outbound ? ">>" : "<<", text);
	it->second.buffer->append (buf);

	// Auto-scroll
	if (it->second.display)
		it->second.display->scroll (it->second.display->count_lines (0, it->second.buffer->length (), true), 0);
}

// ============================================================
// URL Grabber Window Functions
// ============================================================

static void
url_grabber_close_cb (Fl_Widget *, void *)
{
	if (url_grabber_window.window)
	{
		url_grabber_window.window->hide ();
		delete url_grabber_window.window;
		url_grabber_window.window = nullptr;
	}
}

static void
url_grabber_open_cb (Fl_Widget *, void *)
{
	if (!url_grabber_window.list)
		return;

	int sel = url_grabber_window.list->value ();
	if (sel <= 0)
		return;

	const char *url = url_grabber_window.list->text (sel);
	if (url)
		fe_open_url (url);
}

static void
url_grabber_copy_cb (Fl_Widget *, void *)
{
	if (!url_grabber_window.list)
		return;

	int sel = url_grabber_window.list->value ();
	if (sel <= 0)
		return;

	const char *url = url_grabber_window.list->text (sel);
	if (url)
		Fl::copy (url, strlen (url), 1);
}

static void
url_grabber_clear_cb (Fl_Widget *, void *)
{
	if (url_grabber_window.list)
		url_grabber_window.list->clear ();
	url_clear ();
}

static void
url_grabber_save_cb (Fl_Widget *, void *)
{
	const char *filename = fl_file_chooser (_("Save URL List"), "*.txt", nullptr);
	if (filename)
		url_save_tree (filename, "w", TRUE);
}

static int
url_grabber_fill_list (const void *url, void *)
{
	if (url_grabber_window.list && url)
		url_grabber_window.list->add (static_cast<const char *>(url));
	return TRUE;
}

static void
url_grabber_open (void)
{
	if (url_grabber_window.window)
	{
		url_grabber_window.window->show ();
		return;
	}

	Fl_Window *win = new Fl_Window (500, 400, _("URL Grabber - HexChat"));
	url_grabber_window.window = win;

	url_grabber_window.list = new Fl_Select_Browser (10, 10, 480, 320);

	int bx = 10, by = 340;
	url_grabber_window.open_btn = new Fl_Button (bx, by, 90, 25, _("Open"));
	url_grabber_window.open_btn->callback (url_grabber_open_cb);
	bx += 100;

	url_grabber_window.copy_btn = new Fl_Button (bx, by, 90, 25, _("Copy"));
	url_grabber_window.copy_btn->callback (url_grabber_copy_cb);
	bx += 100;

	url_grabber_window.clear_btn = new Fl_Button (bx, by, 90, 25, _("Clear"));
	url_grabber_window.clear_btn->callback (url_grabber_clear_cb);
	bx += 100;

	url_grabber_window.save_btn = new Fl_Button (bx, by, 90, 25, _("Save"));
	url_grabber_window.save_btn->callback (url_grabber_save_cb);

	Fl_Button *close_btn = new Fl_Button (400, 365, 90, 25, _("Close"));
	close_btn->callback (url_grabber_close_cb);

	win->callback (url_grabber_close_cb);
	win->end ();
	win->show ();

	// Fill list from url_tree
	if (url_tree)
		tree_foreach (static_cast<tree *>(url_tree), url_grabber_fill_list, nullptr);
}

// ============================================================
// Notify List Window Functions
// ============================================================

static void
notify_window_close_cb (Fl_Widget *, void *)
{
	if (notify_window.window)
	{
		notify_window.window->hide ();
		delete notify_window.window;
		notify_window.window = nullptr;
	}
}

static void
notify_fill_list (void)
{
	if (!notify_window.list)
		return;

	notify_window.list->clear ();

	GSList *list = notify_list;
	while (list)
	{
		struct notify *notify = static_cast<struct notify *>(list->data);
		if (notify && notify->name)
		{
			char buf[512];
			g_snprintf (buf, sizeof buf, "%s\t%s",
				notify->name,
				notify->networks ? notify->networks : _("All networks"));
			notify_window.list->add (buf, notify);
		}
		list = list->next;
	}
}

static void
notify_add_cb (Fl_Widget *, void *)
{
	if (!notify_window.nick_input)
		return;

	const char *nick = notify_window.nick_input->value ();
	if (!nick || !*nick)
	{
		fl_alert (_("Please enter a nickname."));
		return;
	}

	const char *networks = notify_window.network_input ? notify_window.network_input->value () : "";

	notify_adduser ((char *)nick, (char *)(networks && *networks ? networks : nullptr));
	notify_save ();
	notify_fill_list ();

	notify_window.nick_input->value ("");
	if (notify_window.network_input)
		notify_window.network_input->value ("");
}

static void
notify_remove_cb (Fl_Widget *, void *)
{
	if (!notify_window.list)
		return;

	int sel = notify_window.list->value ();
	if (sel <= 0)
		return;

	void *data = notify_window.list->data (sel);
	struct notify *notify = static_cast<struct notify *>(data);
	if (notify && notify->name)
	{
		notify_deluser (notify->name);
		notify_save ();
		notify_fill_list ();
	}
}

static void
notify_open (void)
{
	if (notify_window.window)
	{
		notify_window.window->show ();
		notify_fill_list ();
		return;
	}

	Fl_Window *win = new Fl_Window (450, 350, _("Notify List - HexChat"));
	notify_window.window = win;

	notify_window.list = new Fl_Select_Browser (10, 10, 430, 200);

	new Fl_Box (10, 220, 60, 25, _("Nick:"));
	notify_window.nick_input = new Fl_Input (75, 220, 150, 25);

	new Fl_Box (10, 250, 60, 25, _("Networks:"));
	notify_window.network_input = new Fl_Input (75, 250, 250, 25);
	notify_window.network_input->tooltip (_("Comma-separated list of networks, or leave blank for all"));

	notify_window.add_btn = new Fl_Button (10, 285, 90, 25, _("Add"));
	notify_window.add_btn->callback (notify_add_cb);

	notify_window.remove_btn = new Fl_Button (110, 285, 90, 25, _("Remove"));
	notify_window.remove_btn->callback (notify_remove_cb);

	Fl_Button *close_btn = new Fl_Button (350, 315, 90, 25, _("Close"));
	close_btn->callback (notify_window_close_cb);

	win->callback (notify_window_close_cb);
	win->end ();
	win->show ();

	notify_fill_list ();
}

// ============================================================
// Ignore List Window Functions
// ============================================================

static void
ignore_window_close_cb (Fl_Widget *, void *)
{
	if (ignore_window.window)
	{
		ignore_window.window->hide ();
		delete ignore_window.window;
		ignore_window.window = nullptr;
	}
}

static void
ignore_fill_list (void)
{
	if (!ignore_window.list)
		return;

	ignore_window.list->clear ();

	GSList *list = ignore_list;
	while (list)
	{
		struct ignore *ig = static_cast<struct ignore *>(list->data);
		if (ig && ig->mask)
		{
			char buf[512];
			char types[64] = "";
			if (ig->type & IG_PRIV) strcat (types, "Priv ");
			if (ig->type & IG_NOTI) strcat (types, "Notice ");
			if (ig->type & IG_CHAN) strcat (types, "Chan ");
			if (ig->type & IG_CTCP) strcat (types, "CTCP ");
			if (ig->type & IG_DCC) strcat (types, "DCC ");
			if (ig->type & IG_INVI) strcat (types, "Invite ");

			g_snprintf (buf, sizeof buf, "%s\t%s", ig->mask, types);
			ignore_window.list->add (buf, ig);
		}
		list = list->next;
	}
}

static void
ignore_add_cb (Fl_Widget *, void *)
{
	if (!ignore_window.mask_input)
		return;

	const char *mask = ignore_window.mask_input->value ();
	if (!mask || !*mask)
	{
		fl_alert (_("Please enter a hostmask."));
		return;
	}

	int type = 0;
	if (ignore_window.ignore_priv && ignore_window.ignore_priv->value ()) type |= IG_PRIV;
	if (ignore_window.ignore_notice && ignore_window.ignore_notice->value ()) type |= IG_NOTI;
	if (ignore_window.ignore_chan && ignore_window.ignore_chan->value ()) type |= IG_CHAN;
	if (ignore_window.ignore_ctcp && ignore_window.ignore_ctcp->value ()) type |= IG_CTCP;
	if (ignore_window.ignore_dcc && ignore_window.ignore_dcc->value ()) type |= IG_DCC;
	if (ignore_window.ignore_invite && ignore_window.ignore_invite->value ()) type |= IG_INVI;

	if (type == 0)
	{
		fl_alert (_("Please select at least one type to ignore."));
		return;
	}

	ignore_add ((char *)mask, type, TRUE);
	ignore_save ();
	ignore_fill_list ();

	ignore_window.mask_input->value ("");
}

static void
ignore_remove_cb (Fl_Widget *, void *)
{
	if (!ignore_window.list)
		return;

	int sel = ignore_window.list->value ();
	if (sel <= 0)
		return;

	void *data = ignore_window.list->data (sel);
	struct ignore *ig = static_cast<struct ignore *>(data);
	if (ig)
	{
		ignore_del (ig->mask, ig);
		ignore_save ();
		ignore_fill_list ();
	}
}

static void
ignore_open (void)
{
	if (ignore_window.window)
	{
		ignore_window.window->show ();
		ignore_fill_list ();
		return;
	}

	Fl_Window *win = new Fl_Window (500, 400, _("Ignore List - HexChat"));
	ignore_window.window = win;

	ignore_window.list = new Fl_Select_Browser (10, 10, 480, 200);

	new Fl_Box (10, 220, 60, 25, _("Mask:"));
	ignore_window.mask_input = new Fl_Input (75, 220, 250, 25);
	ignore_window.mask_input->tooltip (_("e.g., *!*@*.example.com"));

	int y = 255;
	ignore_window.ignore_priv = new Fl_Check_Button (10, y, 100, 25, _("Private"));
	ignore_window.ignore_notice = new Fl_Check_Button (120, y, 100, 25, _("Notice"));
	ignore_window.ignore_chan = new Fl_Check_Button (230, y, 100, 25, _("Channel"));
	y += 25;
	ignore_window.ignore_ctcp = new Fl_Check_Button (10, y, 100, 25, _("CTCP"));
	ignore_window.ignore_dcc = new Fl_Check_Button (120, y, 100, 25, _("DCC"));
	ignore_window.ignore_invite = new Fl_Check_Button (230, y, 100, 25, _("Invite"));

	ignore_window.add_btn = new Fl_Button (10, 320, 90, 25, _("Add"));
	ignore_window.add_btn->callback (ignore_add_cb);

	ignore_window.remove_btn = new Fl_Button (110, 320, 90, 25, _("Remove"));
	ignore_window.remove_btn->callback (ignore_remove_cb);

	Fl_Button *close_btn = new Fl_Button (400, 365, 90, 25, _("Close"));
	close_btn->callback (ignore_window_close_cb);

	win->callback (ignore_window_close_cb);
	win->end ();
	win->show ();

	ignore_fill_list ();
}

// ============================================================
// Ban List Window Functions
// ============================================================

static void
banlist_window_close_cb (Fl_Widget *, void *data)
{
	session *sess = static_cast<session *>(data);
	auto it = banlist_windows.find (sess);
	if (it != banlist_windows.end ())
	{
		if (it->second.window)
		{
			it->second.window->hide ();
			delete it->second.window;
		}
		banlist_windows.erase (it);
	}
}

static void
banlist_add_cb (Fl_Widget *, void *data)
{
	session *sess = static_cast<session *>(data);
	auto it = banlist_windows.find (sess);
	if (it == banlist_windows.end () || !it->second.mask_input)
		return;

	const char *mask = it->second.mask_input->value ();
	if (!mask || !*mask)
	{
		fl_alert (_("Please enter a ban mask."));
		return;
	}

	char buf[512];
	g_snprintf (buf, sizeof buf, "MODE %s +b %s", sess->channel, mask);
	handle_command (sess, buf, FALSE);

	it->second.mask_input->value ("");
}

static void
banlist_remove_cb (Fl_Widget *, void *data)
{
	session *sess = static_cast<session *>(data);
	auto it = banlist_windows.find (sess);
	if (it == banlist_windows.end () || !it->second.list)
		return;

	int sel = it->second.list->value ();
	if (sel <= 0)
		return;

	const char *text = it->second.list->text (sel);
	if (!text)
		return;

	// Extract mask (first field before tab)
	char mask[256];
	const char *p = text;
	int i = 0;
	while (*p && *p != '\t' && i < 255)
		mask[i++] = *p++;
	mask[i] = 0;

	char buf[512];
	g_snprintf (buf, sizeof buf, "MODE %s -b %s", sess->channel, mask);
	handle_command (sess, buf, FALSE);
}

static void
banlist_refresh_cb (Fl_Widget *, void *data)
{
	session *sess = static_cast<session *>(data);
	auto it = banlist_windows.find (sess);
	if (it == banlist_windows.end () || !it->second.list)
		return;

	it->second.list->clear ();

	char buf[512];
	g_snprintf (buf, sizeof buf, "MODE %s +b", sess->channel);
	handle_command (sess, buf, FALSE);
}

static void
banlist_open (session *sess)
{
	if (!sess || !sess->channel[0])
		return;

	auto it = banlist_windows.find (sess);
	if (it != banlist_windows.end () && it->second.window)
	{
		it->second.window->show ();
		banlist_refresh_cb (nullptr, sess);
		return;
	}

	BanListWindow blw;
	blw.sess = sess;

	char title[256];
	g_snprintf (title, sizeof title, _("Ban List for %s - HexChat"), sess->channel);

	Fl_Window *win = new Fl_Window (500, 350, nullptr);
	win->copy_label (title);
	blw.window = win;

	blw.list = new Fl_Select_Browser (10, 10, 480, 220);

	new Fl_Box (10, 240, 60, 25, _("Mask:"));
	blw.mask_input = new Fl_Input (75, 240, 300, 25);
	blw.mask_input->tooltip (_("e.g., *!*@*.example.com"));

	blw.add_btn = new Fl_Button (10, 275, 90, 25, _("Ban"));
	blw.add_btn->callback (banlist_add_cb, sess);

	blw.remove_btn = new Fl_Button (110, 275, 90, 25, _("Unban"));
	blw.remove_btn->callback (banlist_remove_cb, sess);

	blw.refresh_btn = new Fl_Button (210, 275, 90, 25, _("Refresh"));
	blw.refresh_btn->callback (banlist_refresh_cb, sess);

	Fl_Button *close_btn = new Fl_Button (400, 315, 90, 25, _("Close"));
	close_btn->callback (banlist_window_close_cb, sess);

	win->callback (banlist_window_close_cb, sess);
	win->end ();
	win->show ();

	banlist_windows[sess] = blw;

	// Request ban list
	banlist_refresh_cb (nullptr, sess);
}

// ============================================================
// Join Channel Dialog Functions (Per-Server)
// ============================================================

static void
join_dialog_close_cb (Fl_Widget *, void *data)
{
	server *serv = static_cast<server *>(data);
	auto it = join_dialogs.find (serv);
	if (it != join_dialogs.end ())
	{
		if (it->second.window)
		{
			it->second.window->hide ();
			delete it->second.window;
		}
		join_dialogs.erase (it);
	}
}

static void
join_dialog_history_cb (Fl_Widget *, void *data)
{
	server *serv = static_cast<server *>(data);
	auto it = join_dialogs.find (serv);
	if (it == join_dialogs.end ())
		return;

	JoinChannelDialog &jd = it->second;
	if (!jd.history_list || !jd.channel_input)
		return;

	int sel = jd.history_list->value ();
	if (sel <= 0)
		return;

	const char *text = jd.history_list->text (sel);
	if (text)
	{
		// Parse channel and optional key from "channel (key)"
		char chan[256] = {0};
		char key[256] = {0};
		const char *p = text;
		int i = 0;

		// Extract channel name
		while (*p && *p != ' ' && *p != '\t' && i < 255)
			chan[i++] = *p++;
		chan[i] = 0;

		// Check for key in parentheses
		while (*p && (*p == ' ' || *p == '\t'))
			p++;
		if (*p == '(')
		{
			p++;
			i = 0;
			while (*p && *p != ')' && i < 255)
				key[i++] = *p++;
			key[i] = 0;
		}

		jd.channel_input->value (chan);
		if (jd.key_input && key[0])
			jd.key_input->value (key);
	}
}

static void
join_dialog_join_cb (Fl_Widget *, void *data)
{
	server *serv = static_cast<server *>(data);
	auto it = join_dialogs.find (serv);
	if (it == join_dialogs.end ())
		return;

	JoinChannelDialog &jd = it->second;
	if (!jd.channel_input)
		return;

	const char *channel = jd.channel_input->value ();
	const char *key = jd.key_input ? jd.key_input->value () : "";

	if (!channel || !*channel)
	{
		fl_alert (_("Please enter a channel name."));
		return;
	}

	// Ensure channel starts with # or &
	std::string chan_str;
	if (channel[0] != '#' && channel[0] != '&' && channel[0] != '+' && channel[0] != '!')
		chan_str = "#";
	chan_str += channel;

	// Save show_on_connect preference
	if (jd.show_on_connect)
		prefs.hex_gui_join_dialog = jd.show_on_connect->value ();

	// Join the channel
	if (serv && serv->server_session)
	{
		char buf[512];
		if (key && *key)
			g_snprintf (buf, sizeof buf, "JOIN %s %s", chan_str.c_str (), key);
		else
			g_snprintf (buf, sizeof buf, "JOIN %s", chan_str.c_str ());
		handle_command (serv->server_session, buf, FALSE);
	}

	// Close the dialog
	join_dialog_close_cb (nullptr, serv);
}

static void
join_dialog_chanlist_cb (Fl_Widget *, void *data)
{
	server *serv = static_cast<server *>(data);

	// Save show_on_connect preference
	auto it = join_dialogs.find (serv);
	if (it != join_dialogs.end () && it->second.show_on_connect)
		prefs.hex_gui_join_dialog = it->second.show_on_connect->value ();

	// Close join dialog and open channel list
	join_dialog_close_cb (nullptr, serv);

	// Open channel list for this server
	if (serv && serv->server_session)
	{
		char buf[64];
		g_snprintf (buf, sizeof buf, "LIST");
		handle_command (serv->server_session, buf, FALSE);
	}
}

static void
join_dialog_populate_history (JoinChannelDialog &jd, server *serv)
{
	if (!jd.history_list || !serv)
		return;

	jd.history_list->clear ();

	// Add channels from network's favorite channels
	ircnet *net = static_cast<ircnet *>(serv->network);
	if (net && net->favchanlist)
	{
		for (GSList *li = net->favchanlist; li; li = li->next)
		{
			favchannel *fav = static_cast<favchannel *>(li->data);
			if (fav && fav->name)
			{
				char buf[512];
				if (fav->key && fav->key[0])
					g_snprintf (buf, sizeof buf, "%s (%s)", fav->name, fav->key);
				else
					g_snprintf (buf, sizeof buf, "%s", fav->name);
				jd.history_list->add (buf);
			}
		}
	}

	// Add recently joined channels from session list
	GSList *slist = sess_list;
	std::set<std::string> added;
	while (slist)
	{
		session *sess = static_cast<session *>(slist->data);
		if (sess && sess->server == serv && sess->type == SESS_CHANNEL && sess->channel[0])
		{
			std::string chan = sess->channel;
			if (added.find (chan) == added.end ())
			{
				jd.history_list->add (sess->channel);
				added.insert (chan);
			}
		}
		slist = slist->next;
	}

	// Add common default channels if list is empty
	if (jd.history_list->size () == 0)
	{
		jd.history_list->add ("#help");
		jd.history_list->add ("#chat");
	}
}

static void
joind_open (server *serv)
{
	if (!serv)
		return;

	// Check if dialog already exists for this server
	auto it = join_dialogs.find (serv);
	if (it != join_dialogs.end () && it->second.window)
	{
		it->second.window->show ();
		return;
	}

	JoinChannelDialog &jd = join_dialogs[serv];
	jd.serv = serv;

	char title[256];
	ircnet *net = static_cast<ircnet *>(serv->network);
	const char *netname = net ? net->name : serv->servername;
	g_snprintf (title, sizeof title, _("Join Channel - %s"), netname ? netname : "Unknown");

	Fl_Window *win = new Fl_Window (400, 350, title);
	jd.window = win;

	int y = 10;

	// Instructions
	Fl_Box *info = new Fl_Box (10, y, 380, 40,
		_("Enter a channel name to join, or select\nfrom your favorites/recent channels:"));
	info->align (FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
	y += 50;

	// Channel name input
	new Fl_Box (10, y, 80, 25, _("Channel:"));
	jd.channel_input = new Fl_Input (100, y, 200, 25);
	jd.channel_input->tooltip (_("Channel name (e.g., #channel)"));
	y += 30;

	// Key input (for password-protected channels)
	new Fl_Box (10, y, 80, 25, _("Key:"));
	jd.key_input = new Fl_Input (100, y, 200, 25);
	jd.key_input->type (FL_SECRET_INPUT);
	jd.key_input->tooltip (_("Channel key/password (optional)"));
	y += 35;

	// History/favorites list
	new Fl_Box (10, y, 200, 20, _("Favorites / Recent Channels:"));
	y += 22;
	jd.history_list = new Fl_Hold_Browser (10, y, 380, 120);
	jd.history_list->callback (join_dialog_history_cb, serv);
	y += 130;

	// Show on connect checkbox
	jd.show_on_connect = new Fl_Check_Button (10, y, 250, 25, _("Show this dialog on connect"));
	jd.show_on_connect->value (prefs.hex_gui_join_dialog);
	y += 35;

	// Buttons
	jd.join_btn = new Fl_Return_Button (10, y, 100, 30, _("Join"));
	jd.join_btn->callback (join_dialog_join_cb, serv);

	jd.chanlist_btn = new Fl_Button (120, y, 120, 30, _("Channel List..."));
	jd.chanlist_btn->callback (join_dialog_chanlist_cb, serv);

	Fl_Button *cancel_btn = new Fl_Button (310, y, 80, 30, _("Cancel"));
	cancel_btn->callback (join_dialog_close_cb, serv);

	// Populate the history list
	join_dialog_populate_history (jd, serv);

	win->callback (join_dialog_close_cb, serv);
	win->set_modal ();
	win->end ();
	win->show ();

	// Focus the channel input
	jd.channel_input->take_focus ();
}

// Clean up join dialogs when server disconnects
static void
joind_server_cleanup (server *serv)
{
	auto it = join_dialogs.find (serv);
	if (it != join_dialogs.end ())
	{
		if (it->second.window)
		{
			it->second.window->hide ();
			delete it->second.window;
		}
		join_dialogs.erase (it);
	}
}

// ============================================================
// Channel List Window Functions
// ============================================================

static void
chanlist_window_close_cb (Fl_Widget *, void *data)
{
	server *serv = static_cast<server *>(data);
	auto it = chanlist_windows.find (serv);
	if (it != chanlist_windows.end ())
	{
		if (it->second.window)
		{
			it->second.window->hide ();
			delete it->second.window;
		}
		chanlist_windows.erase (it);
	}
}

static void
chanlist_join_cb (Fl_Widget *, void *data)
{
	server *serv = static_cast<server *>(data);
	auto it = chanlist_windows.find (serv);
	if (it == chanlist_windows.end () || !it->second.list)
		return;

	int sel = it->second.list->value ();
	if (sel <= 0)
		return;

	const char *text = it->second.list->text (sel);
	if (!text)
		return;

	// Extract channel name (first word)
	char chan[256];
	const char *p = text;
	int i = 0;
	while (*p && *p != '\t' && *p != ' ' && i < 255)
		chan[i++] = *p++;
	chan[i] = 0;

	if (chan[0] && serv->server_session)
	{
		char buf[512];
		g_snprintf (buf, sizeof buf, "JOIN %s", chan);
		handle_command (serv->server_session, buf, FALSE);
	}
}

static void
chanlist_refresh_cb (Fl_Widget *, void *data)
{
	server *serv = static_cast<server *>(data);
	auto it = chanlist_windows.find (serv);
	if (it == chanlist_windows.end ())
		return;

	it->second.list->clear ();
	it->second.channels_found = 0;
	it->second.channels_shown = 0;
	it->second.users_found = 0;
	it->second.users_shown = 0;

	if (serv->connected)
	{
		static char empty_arg[] = "";
		serv->p_list_channels (serv, empty_arg, 1);
	}
}

static void
chanlist_update_info (ChanListWindow *clw)
{
	if (!clw || !clw->info_label)
		return;
	char buf[256];
	g_snprintf (buf, sizeof buf, _("Showing %d/%d channels, %d/%d users"),
		clw->channels_shown, clw->channels_found,
		clw->users_shown, clw->users_found);
	clw->info_label->copy_label (buf);
}

static void
chanlist_open (server *serv, int do_refresh)
{
	auto it = chanlist_windows.find (serv);
	if (it != chanlist_windows.end () && it->second.window)
	{
		it->second.window->show ();
		if (do_refresh)
			chanlist_refresh_cb (nullptr, serv);
		return;
	}

	ChanListWindow clw;
	clw.serv = serv;

	char title[256];
	const char *srv_name = (serv && serv->servername[0]) ? serv->servername : _("Server");
	g_snprintf (title, sizeof title, _("Channel List (%s) - FlexChat"),
		srv_name);

	Fl_Window *win = new Fl_Window (640, 480, nullptr);
	win->copy_label (title);
	clw.window = win;

	clw.info_label = new Fl_Box (10, 10, 620, 20, _("Channel list not yet loaded"));
	clw.info_label->align (FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	clw.list = new ChannelListBrowser (10, 35, 620, 350, serv);

	// Filter controls
	new Fl_Box (10, 395, 40, 25, _("Find:"));
	clw.filter_input = new Fl_Input (55, 395, 150, 25);

	new Fl_Box (220, 395, 30, 25, _("Min:"));
	clw.min_users = new Fl_Spinner (255, 395, 60, 25);
	clw.min_users->minimum (1);
	clw.min_users->maximum (99999);
	clw.min_users->value (1);

	new Fl_Box (325, 395, 30, 25, _("Max:"));
	clw.max_users = new Fl_Spinner (360, 395, 60, 25);
	clw.max_users->minimum (1);
	clw.max_users->maximum (99999);
	clw.max_users->value (99999);

	clw.match_channel = new Fl_Check_Button (430, 395, 90, 25, _("Channel"));
	clw.match_channel->value (1);
	clw.match_topic = new Fl_Check_Button (525, 395, 70, 25, _("Topic"));
	clw.match_topic->value (1);

	// Buttons
	clw.refresh_btn = new Fl_Button (10, 430, 100, 30, _("Refresh"));
	clw.refresh_btn->callback (chanlist_refresh_cb, serv);

	clw.join_btn = new Fl_Button (120, 430, 100, 30, _("Join"));
	clw.join_btn->callback (chanlist_join_cb, serv);

	win->callback (chanlist_window_close_cb, serv);
	win->end ();
	win->show ();

	chanlist_windows[serv] = clw;

	if (do_refresh)
		chanlist_refresh_cb (nullptr, serv);
}

// ============================================================
// Input history navigation
// ============================================================

static void
add_to_history (const char *text)
{
	if (!text || !*text)
		return;

	// Don't add duplicates of the last entry
	if (!input_history.empty () && input_history.back () == text)
		return;

	input_history.push_back (text);
	if ((int)input_history.size () > MAX_HISTORY)
		input_history.erase (input_history.begin ());

	history_pos = -1;
}

static void
history_up ()
{
	if (input_history.empty ())
		return;

	if (history_pos < 0)
		history_pos = (int)input_history.size () - 1;
	else if (history_pos > 0)
		history_pos--;

	if (input_box && history_pos >= 0 && history_pos < (int)input_history.size ())
		input_box->value (input_history[history_pos].c_str ());
}

static void
history_down ()
{
	if (input_history.empty () || history_pos < 0)
		return;

	history_pos++;
	if (history_pos >= (int)input_history.size ())
	{
		history_pos = -1;
		if (input_box)
			input_box->value ("");
	}
	else if (input_box)
	{
		input_box->value (input_history[history_pos].c_str ());
	}
}

// ============================================================
// GLib integration
// ============================================================

static void
glib_iteration_cb (void *data)
{
	(void)data;
	// Run a bounded number of pending GLib iterations, then reschedule
	int processed = 0;
	while (g_main_context_pending (nullptr) && processed < 5)
	{
		g_main_context_iteration (nullptr, FALSE);
		processed++;
	}
	if (processed && fltk_debug)
		debug_log ("glib_iteration_cb processed=%d", processed);
	Fl::repeat_timeout (0.02, glib_iteration_cb, nullptr);
}

static void
send_input_cb (Fl_Widget *, void *)
{
	const char *val = input_box ? input_box->value () : "";
	if (!val || !*val)
		return;

	add_to_history (val);
	handle_multiline (current_tab, (char *)val, TRUE, FALSE);
	if (input_box)
	{
		input_box->value ("");
		input_box->take_focus ();
	}
}

// Input box keyboard handler for history navigation
static int
input_box_handler (int event)
{
	if (event == FL_KEYBOARD && input_box && Fl::focus () == input_box)
	{
		int key = Fl::event_key ();
		if (key == FL_Up)
		{
			history_up ();
			return 1;
		}
		else if (key == FL_Down)
		{
			history_down ();
			return 1;
		}
		// Ctrl+K - clear line
		else if (key == 'k' && Fl::event_ctrl ())
		{
			if (input_box)
				input_box->value ("");
			return 1;
		}
		// Ctrl+L - lastlog
		else if (key == 'l' && Fl::event_ctrl ())
		{
			menu_search_cb (nullptr, nullptr);
			return 1;
		}
	}
	return 0;
}

extern "C" {

// FLTK frontend doesn't expose extra CLI options, but GLib expects a non-null table.
static const GOptionEntry fltk_option_entries[] = {
	{ nullptr }
};

int
fe_args (int argc, char *argv[])
{
	GError *error = NULL;
	GOptionContext *context;

	setlocale (LC_ALL, "");

#ifdef ENABLE_NLS
	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);
#endif

	if (getenv ("HEXCHAT_FLTK_DEBUG"))
		fltk_debug = true;

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, fltk_option_entries, GETTEXT_PACKAGE);
	g_option_context_parse (context, &argc, &argv, &error);
	g_option_context_free (context);

	if (error)
	{
		if (error->message)
			printf ("%s\n", error->message);
		return 1;
	}

	return -1;
}

void
fe_init (void)
{
	main_win = new Fl_Window (1000, 750, "HexChat (FLTK)");

	// Menu bar
	menu_bar = new Fl_Menu_Bar (0, 0, 1000, 25);
	menu_bar->add (_("&HexChat/&Server List..."), FL_CTRL + 's', menu_server_connect_cb);
	menu_bar->add (_("&HexChat/&Disconnect"), 0, menu_server_disconnect_cb);
	menu_bar->add (_("&HexChat/&Reconnect"), FL_CTRL + 'r', menu_server_reconnect_cb);
	menu_bar->add (_("&HexChat/Join &Channel..."), FL_CTRL + 'j', menu_join_channel_cb);
	menu_bar->add (_("&HexChat/"), 0, nullptr, nullptr, FL_MENU_DIVIDER);
	menu_bar->add (_("&HexChat/Change &Nick..."), FL_CTRL + 'n', menu_nick_cb);
	menu_bar->add (_("&HexChat/Set &Away..."), 0, menu_away_cb);
	menu_bar->add (_("&HexChat/Set &Back"), 0, menu_back_cb);
	menu_bar->add (_("&HexChat/"), 0, nullptr, nullptr, FL_MENU_DIVIDER);
	menu_bar->add (_("&HexChat/&Invisible Mode"), 0, menu_invisible_cb, nullptr, FL_MENU_TOGGLE);
	menu_bar->add (_("&HexChat/Receive Server &Notices"), 0, menu_receive_notices_cb, nullptr, FL_MENU_TOGGLE);
	menu_bar->add (_("&HexChat/Receive &Wallops"), 0, menu_receive_wallops_cb, nullptr, FL_MENU_TOGGLE);
	menu_bar->add (_("&HexChat/"), 0, nullptr, nullptr, FL_MENU_DIVIDER);
	menu_bar->add (_("&HexChat/&Quit"), FL_CTRL + 'q', menu_quit_cb);

	menu_bar->add (_("&View/&Clear Text"), FL_CTRL + 'k', menu_clear_cb);
	menu_bar->add (_("&View/&Search..."), FL_CTRL + 'f', menu_search_cb);
	menu_bar->add (_("&View/&Save Text..."), 0, menu_save_text_cb);

	menu_bar->add (_("&Server/&Join Channel..."), FL_CTRL + 'j', menu_join_channel_cb);
	menu_bar->add (_("&Server/&Channel List..."), 0, menu_chanlist_cb);
	menu_bar->add (_("&Server/&Raw Log..."), 0, menu_rawlog_cb);
	menu_bar->add (_("&Server/&URL Grabber..."), 0, menu_url_grabber_cb);
	menu_bar->add (_("&Server/"), 0, nullptr, nullptr, FL_MENU_DIVIDER);
	menu_bar->add (_("&Server/&Disconnect"), 0, menu_server_disconnect_cb);
	menu_bar->add (_("&Server/&Reconnect"), 0, menu_server_reconnect_cb);

	menu_bar->add (_("&Window/DCC &Transfers..."), 0, menu_dcc_recv_cb);
	menu_bar->add (_("&Window/DCC C&hat List..."), 0, menu_dcc_chat_cb);
	menu_bar->add (_("&Window/"), 0, nullptr, nullptr, FL_MENU_DIVIDER);
	menu_bar->add (_("&Window/&Notify List..."), 0, menu_notify_list_cb);
	menu_bar->add (_("&Window/&Ignore List..."), 0, menu_ignore_list_cb);
	menu_bar->add (_("&Window/&Ban List..."), 0, menu_ban_list_cb);
	menu_bar->add (_("&Window/"), 0, nullptr, nullptr, FL_MENU_DIVIDER);
	menu_bar->add (_("&Window/&Close Tab"), FL_CTRL + 'w', menu_part_channel_cb);

	menu_bar->add (_("&Settings/&Preferences..."), 0, menu_prefs_cb);

	menu_bar->add (_("&Help/&About..."), 0, menu_about_cb);

	// Tab widget
	session_tree = new Fl_Tree (10, 30, 170, 620);
	session_tree->showroot (0);
	session_tree->selectmode (FL_TREE_SELECT_SINGLE);
	session_tree->callback (session_tree_cb);

	content_stack = new Fl_Group (190, 30, 800, 620);
	content_stack->end ();

	// Input box and send button (with spell checking support)
	input_box = new SpellInput (190, 660, 620, 30);
	input_box->when (FL_WHEN_ENTER_KEY_ALWAYS);
	input_box->callback (send_input_cb);

	// Initialize spell checking
	initialize_enchant ();
	if (have_enchant && prefs.hex_gui_input_spell)
		spell_init_broker ();
	send_button = new Fl_Button (820, 660, 170, 30, _("Send"));
	send_button->callback (send_input_cb);

	// Enhanced status bar with lag/throttle/count indicators
	status_bar = new Fl_Box (190, 700, 500, 25, _("Ready"));
	status_bar->box (FL_FLAT_BOX);
	status_bar->align (FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	lag_indicator = new Fl_Progress (700, 705, 90, 15);
	lag_indicator->minimum (0);
	lag_indicator->maximum (1000); // ms scale
	lag_indicator->color (FL_DARK1);
	lag_indicator->selection_color (FL_GREEN);
	lag_indicator->labelsize (10);
	lag_indicator->copy_label ("");

	throttle_indicator = new Fl_Progress (800, 705, 90, 15);
	throttle_indicator->minimum (0);
	throttle_indicator->maximum (1);
	throttle_indicator->color (FL_DARK1);
	throttle_indicator->selection_color (FL_RED);
	throttle_indicator->copy_label ("");
	throttle_indicator->labelsize (10);

	user_count_label = new Fl_Box (900, 700, 130, 25, "");
	user_count_label->box (FL_FLAT_BOX);
	user_count_label->align (FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

	main_win->resizable (content_stack);
	main_win->end ();
	main_win->show ();

	// Register keyboard handler for input history
	Fl::add_handler (input_box_handler);

	// Pump GLib via a short repeating timer
	Fl::add_timeout (0.02, glib_iteration_cb, nullptr);

	// Apply global font preference to input widgets
	std::string fname;
	int fsize;
	parse_font_spec (prefs.hex_text_font_main, fname, fsize);
	apply_font_to_widgets (fname, fsize);
	set_status (_("Ready"));
}

void
fe_main (void)
{
	Fl::run ();
}

void
fe_cleanup (void)
{
	spell_cleanup ();
}

void
fe_exit (void)
{
	if (main_win)
		main_win->hide ();
	Fl::awake ();
}

int
fe_timeout_add (int interval, void *callback, void *userdata)
{
	if (fltk_debug)
		debug_log ("fe_timeout_add interval=%d cb=%p ud=%p", interval, callback, userdata);
	return g_timeout_add (interval, (GSourceFunc)callback, userdata);
}

int
fe_timeout_add_seconds (int interval, void *callback, void *userdata)
{
	if (fltk_debug)
		debug_log ("fe_timeout_add_seconds interval=%d cb=%p ud=%p", interval, callback, userdata);
	return g_timeout_add_seconds (interval, (GSourceFunc)callback, userdata);
}

void
fe_timeout_remove (int tag)
{
	if (fltk_debug)
		debug_log ("fe_timeout_remove tag=%d", tag);
	g_source_remove (tag);
}

int
fe_input_add (int sok, int flags, void *func, void *data)
{
	int tag;
	guint type = 0;
	GIOChannel *channel;
	if (fltk_debug)
		debug_log ("fe_input_add fd=%d flags=0x%x cb=%p ud=%p", sok, flags, func, data);

	if (flags & FIA_FD)
		channel = g_io_channel_unix_new (sok);
	else
		channel = g_io_channel_unix_new (sok);

	if (flags & FIA_READ)
		type |= G_IO_IN | G_IO_HUP | G_IO_ERR;
	if (flags & FIA_WRITE)
		type |= G_IO_OUT | G_IO_ERR;
	if (flags & FIA_EX)
		type |= G_IO_PRI;

	tag = g_io_add_watch (channel, (GIOCondition)type, (GIOFunc) func, data);
	g_io_channel_unref (channel);

	return tag;
}

void
fe_input_remove (int tag)
{
	if (fltk_debug)
		debug_log ("fe_input_remove tag=%d", tag);
	g_source_remove (tag);
}

void
fe_idle_add (void *func, void *data)
{
	g_idle_add ((GSourceFunc)func, data);
}

void
fe_new_window (struct session *sess, int focus)
{
	current_sess = sess;
	current_tab = sess;
	SessionUI *ui = ensure_session_ui (sess);
	debug_log ("fe_new_window channel=%s focus=%d", sess ? sess->channel : "(null)", focus);

	if (ui && focus)
		show_session_content (sess);

	char buf[256];
	g_snprintf (buf, sizeof buf, "%s: %s", _("Opened"),
		sess->channel[0] ? sess->channel : _("server"));
	append_text (sess, buf);
	update_tab_title (sess);
	if (main_win)
		main_win->show ();
	set_status (buf);
}

void
fe_new_server (struct server *serv)
{
	(void)serv;
	debug_log ("fe_new_server");
	append_text (current_tab, _("New server connected"));
	set_status (_("Connected"));
}

void
fe_add_rawlog (struct server *serv, char *text, int len, int outbound)
{
	(void)len;
	rawlog_append (serv, text, outbound);
}

void
fe_message (char *msg, int flags)
{
	(void)flags;
	append_text (current_tab, msg);
}

void
fe_set_topic (struct session *sess, char *topic, char *stripped_topic)
{
	SessionUI *ui = ensure_session_ui (sess);
	if (!ui)
		return;
	const char *src = stripped_topic ? stripped_topic : (topic ? topic : "");
	if (ui->topic)
		ui->topic->label (src ? src : "");
	if (ui->tab)
		ui->tab->tooltip (src ? src : "");
	if (src)
		set_status (src);
	fe_buttons_update (sess);
}

void
fe_set_tab_color (struct session *sess, tabcolor col)
{
	(void)sess;
	(void)col;
}

void
fe_flash_window (struct session *sess)
{
	(void)sess;
	fl_beep ();
}

void
fe_update_mode_buttons (struct session *sess, char mode, char sign)
{
	(void)mode;
	(void)sign;
	if (sess)
		fe_buttons_update (sess);
}

void
fe_update_channel_key (struct session *sess)
{
	if (sess)
	{
		if (sess->channelkey[0])
		{
			char buf[128];
			g_snprintf (buf, sizeof buf, _("Channel key: %s"), sess->channelkey);
			set_status (buf);
		}
		else
		{
			set_status (_("Channel key cleared"));
		}
	}
}

void
fe_update_channel_limit (struct session *sess)
{
	if (sess)
	{
		char buf[128];
		if (sess->limit > 0)
			g_snprintf (buf, sizeof buf, _("Channel limit: %d"), sess->limit);
		else
			g_snprintf (buf, sizeof buf, _("Channel limit removed"));
		set_status (buf);
	}
}

int
fe_is_chanwindow (struct server *serv)
{
	auto it = chanlist_windows.find (serv);
	return (it != chanlist_windows.end () && it->second.window) ? 1 : 0;
}

void
fe_add_chan_list (struct server *serv, char *chan, char *users, char *topic)
{
	auto it = chanlist_windows.find (serv);
	if (it == chanlist_windows.end () || !it->second.list)
		return;

	ChanListWindow *clw = &it->second;
	int num_users = users ? atoi (users) : 0;

	clw->channels_found++;
	clw->users_found += num_users;

	// Apply filters
	int min_u = clw->min_users ? (int)clw->min_users->value () : 1;
	int max_u = clw->max_users ? (int)clw->max_users->value () : 99999;

	if (num_users < min_u || num_users > max_u)
	{
		chanlist_update_info (clw);
		return;
	}

	// Apply text filter
	const char *filter = clw->filter_input ? clw->filter_input->value () : "";
	if (filter && *filter)
	{
		bool match_chan = clw->match_channel ? clw->match_channel->value () : true;
		bool match_top = clw->match_topic ? clw->match_topic->value () : true;
		bool found = false;

		if (match_chan && chan && strcasestr (chan, filter))
			found = true;
		if (match_top && topic && strcasestr (topic, filter))
			found = true;

		if (!found)
		{
			chanlist_update_info (clw);
			return;
		}
	}

	// Add to list
	char buf[1024];
	g_snprintf (buf, sizeof buf, "%s\t%s\t%s",
		chan ? chan : "",
		users ? users : "",
		topic ? topic : "");
	clw->list->add (buf);

	clw->channels_shown++;
	clw->users_shown += num_users;
	chanlist_update_info (clw);
}

void
fe_chan_list_end (struct server *serv)
{
	auto it = chanlist_windows.find (serv);
	if (it != chanlist_windows.end () && it->second.refresh_btn)
		it->second.refresh_btn->activate ();
}

gboolean
fe_add_ban_list (struct session *sess, char *mask, char *who, char *when, int rplcode)
{
	(void)rplcode;

	// Add to ban list window if open
	auto it = banlist_windows.find (sess);
	if (it != banlist_windows.end () && it->second.list)
	{
		char buf[512];
		g_snprintf (buf, sizeof buf, "%s\t%s\t%s",
			mask ? mask : "",
			who ? who : "",
			when ? when : "");
		it->second.list->add (buf);
	}

	return FALSE;
}

gboolean
fe_ban_list_end (struct session *sess, int rplcode)
{
	(void)rplcode;
	append_text (sess, _("Ban list end"));
	return FALSE;
}

void
fe_notify_update (char *name)
{
	char buf[256];
	g_snprintf (buf, sizeof buf, _("Notify: %s changed state"), name ? name : "");
	append_text (current_tab, buf);
}

void
fe_notify_ask (char *name, char *networks)
{
	char buf[256];
	g_snprintf (buf, sizeof buf, _("Add %s to notify list? %s"), name ? name : "", networks ? networks : "");
	append_text (current_tab, buf);
}

void
fe_text_clear (struct session *sess, int lines)
{
	(void)lines;
	SessionUI *ui = ensure_session_ui (sess);
	if (ui && ui->buffer)
		ui->buffer->text ("");
}

void
fe_close_window (struct session *sess)
{
	auto it = session_ui_map.find (sess);
	if (it == session_ui_map.end ())
		return;
	debug_log ("fe_close_window channel=%s", sess ? sess->channel : "(null)");

	SessionUI &ui = it->second;
	if (ui.tab && content_stack)
		content_stack->remove (ui.tab);
	delete ui.tab;
	session_ui_map.erase (it);

	if (current_tab == sess)
	{
		current_tab = nullptr;
		current_sess = nullptr;
		if (!session_ui_map.empty ())
		{
			current_tab = session_ui_map.begin ()->first;
			current_sess = current_tab;
			show_session_content (current_tab);
		}
	}
}

void
fe_progressbar_start (struct session *sess)
{
	append_text (sess, _("Progress started"));
	set_status (_("Progress started"));
}

void
fe_progressbar_end (struct server *serv)
{
	(void)serv;
	append_text (current_tab, _("Progress finished"));
	set_status (_("Progress finished"));
}

void
fe_print_text (struct session *sess, char *text, time_t stamp, gboolean no_activity)
{
	(void)stamp; (void)no_activity;
	append_text (sess, text);
}

void
fe_userlist_insert (struct session *sess, struct User *newuser, gboolean sel)
{
	(void)sel;
	if (!newuser)
		return;
	SessionUI *ui = ensure_session_ui (sess);
	if (!ui)
		return;
	std::string nick (newuser->nick);
	std::string label;
	if (newuser->prefix[0])
		label += newuser->prefix;
	label += nick;
	ui->users[nick] = label;
	ui->userlist_dirty = true;
	schedule_userlist_refresh ();
}

int
fe_userlist_remove (struct session *sess, struct User *user)
{
	if (!user)
		return 0;
	SessionUI *ui = ensure_session_ui (sess);
	if (!ui)
		return 0;
	ui->users.erase (user->nick);
	ui->userlist_dirty = true;
	schedule_userlist_refresh ();
	return 1;
}

void
fe_userlist_rehash (struct session *sess, struct User *user)
{
	(void)user;
	SessionUI *ui = ensure_session_ui (sess);
	if (!ui)
		return;
	ui->userlist_dirty = true;
	schedule_userlist_refresh ();
}

void
fe_userlist_update (struct session *sess, struct User *user)
{
	if (!user)
		return;
	fe_userlist_insert (sess, user, FALSE);
}

void
fe_userlist_numbers (struct session *sess)
{
	SessionUI *ui = ensure_session_ui (sess);
	if (!ui || !ui->user_browser)
		return;

	int count = ui->user_browser->size ();
	char buf[128];

	// Count ops and voiced users
	int ops = 0, voiced = 0;
	for (auto &entry : ui->users)
	{
		if (!entry.second.empty ())
		{
			char prefix = entry.second[0];
			if (prefix == '@' || prefix == '&' || prefix == '~')
				ops++;
			else if (prefix == '+')
				voiced++;
		}
	}

	g_snprintf (buf, sizeof buf, _("Users: %d (%d ops, %d voiced)"), count, ops, voiced);

	if (user_count_label && sess == current_sess)
		user_count_label->copy_label (buf);
}

void
fe_userlist_clear (struct session *sess)
{
	SessionUI *ui = ensure_session_ui (sess);
	if (!ui)
		return;
	ui->users.clear ();
	ui->userlist_dirty = true;
	schedule_userlist_refresh ();
}

void
fe_userlist_set_selected (struct session *sess)
{
	(void)sess;
}

void
fe_uselect (session *sess, char *word[], int do_clear, int scroll_to)
{
	(void)scroll_to;
	SessionUI *ui = ensure_session_ui (sess);
	if (!ui || !ui->user_browser)
		return;
	if (do_clear)
		ui->user_browser->deselect ();
	for (int i = 0; word && word[i]; i++)
	{
		std::string target (word[i]);
		int idx = 1;
		for (auto &entry : ui->users)
		{
			if (entry.first == target)
				ui->user_browser->select (idx);
			idx++;
		}
	}
}

void
fe_dcc_add (struct DCC *dcc)
{
	if (!dcc)
		return;

	switch (dcc->type)
	{
	case TYPE_RECV:
	case TYPE_SEND:
		if (dcc_file_window.window)
			dcc_fill_list (&dcc_file_window, false);
		break;
	case TYPE_CHATSEND:
	case TYPE_CHATRECV:
		if (dcc_chat_window.window)
			dcc_fill_list (&dcc_chat_window, true);
		break;
	}
}

void
fe_dcc_update (struct DCC *dcc)
{
	if (!dcc)
		return;

	switch (dcc->type)
	{
	case TYPE_RECV:
	case TYPE_SEND:
		if (dcc_file_window.window)
			dcc_fill_list (&dcc_file_window, false);
		break;
	case TYPE_CHATSEND:
	case TYPE_CHATRECV:
		if (dcc_chat_window.window)
			dcc_fill_list (&dcc_chat_window, true);
		break;
	}
}

void
fe_dcc_remove (struct DCC *dcc)
{
	if (!dcc)
		return;

	switch (dcc->type)
	{
	case TYPE_RECV:
	case TYPE_SEND:
		if (dcc_file_window.window)
			dcc_fill_list (&dcc_file_window, false);
		break;
	case TYPE_CHATSEND:
	case TYPE_CHATRECV:
		if (dcc_chat_window.window)
			dcc_fill_list (&dcc_chat_window, true);
		break;
	}
}

int
fe_dcc_open_recv_win (int passive)
{
	dcc_open_file_window (passive);
	return dcc_file_window.window ? TRUE : FALSE;
}

int
fe_dcc_open_send_win (int passive)
{
	dcc_open_file_window (passive);
	return dcc_file_window.window ? TRUE : FALSE;
}

int
fe_dcc_open_chat_win (int passive)
{
	dcc_open_chat_window (passive);
	return dcc_chat_window.window ? TRUE : FALSE;
}

void
fe_clear_channel (struct session *sess)
{
	fe_text_clear (sess, 0);
	set_status (_("Channel cleared"));
}

void
fe_session_callback (struct session *sess)
{
	(void)sess;
	set_status (_("Session callback"));
}

void
fe_server_callback (struct server *serv)
{
	(void)serv;
	set_status (_("Server callback"));
}

void
fe_url_add (const char *text)
{
	// Add to URL grabber window if open
	if (url_grabber_window.window && url_grabber_window.list && text)
		url_grabber_window.list->add (text);
}

void
fe_buttons_update (struct session *sess)
{
	SessionUI *ui = ensure_session_ui (sess);
	if (!ui)
		return;

	bool in_chan = sess && sess->channel[0];
	bool can_manage = in_chan && sess_can_manage (sess);
	bool can_voice = in_chan && sess_has_voice (sess);

	if (ui->toolbar)
	{
		if (in_chan)
			ui->toolbar->activate ();
		else
			ui->toolbar->deactivate ();
	}

	if (ui->op_btn)
		can_manage ? ui->op_btn->activate () : ui->op_btn->deactivate ();
	if (ui->ban_btn)
		can_manage ? ui->ban_btn->activate () : ui->ban_btn->deactivate ();
	if (ui->kick_btn)
		can_manage ? ui->kick_btn->activate () : ui->kick_btn->deactivate ();
	if (ui->voice_btn)
		can_voice ? ui->voice_btn->activate () : ui->voice_btn->deactivate ();
}

void
fe_dlgbuttons_update (struct session *sess)
{
	(void)sess;
}

void
fe_dcc_send_filereq (struct session *sess, char *nick, int maxcps, int passive)
{
	if (!sess || !nick)
		return;

	char title[256];
	g_snprintf (title, sizeof title, _("Send file to %s"), nick);

	const char *path = fl_file_chooser (title, nullptr, nullptr);
	if (path)
		dcc_send (sess, nick, (char *)path, maxcps, passive);
}

void
fe_set_channel (struct session *sess)
{
	update_tab_title (sess);
	set_status (sess && sess->channel[0] ? sess->channel : _("server"));
}

void
fe_set_title (struct session *sess)
{
	if (!main_win || !sess)
		return;

	if (sess->channel[0] && sess->server && sess->server->servername[0])
	{
		char buf[256];
		g_snprintf (buf, sizeof buf, "%s — %s", sess->channel, sess->server->servername);
		main_win->label (buf);
	}
	else if (sess->channel[0])
	{
		main_win->label (sess->channel);
	}
}

void
fe_set_nonchannel (struct session *sess, int state)
{
	SessionUI *ui = ensure_session_ui (sess);
	if (ui && ui->toolbar)
	{
		if (state)
			ui->toolbar->deactivate ();
		else
			ui->toolbar->activate ();
	}
	set_status (state ? _("Nonchannel state updated") : _("Channel state updated"));
}

void
fe_set_nick (struct server *serv, char *newnick)
{
	if (newnick)
	{
		append_text (current_tab, newnick);
		set_status (newnick);
		if (serv && serv->front_session)
			update_tab_title (serv->front_session);
		if (main_win && serv && serv->front_session && serv->front_session->channel[0])
		{
			char buf[256];
			g_snprintf (buf, sizeof buf, "%s — %s", serv->front_session->channel, newnick);
			main_win->label (buf);
		}
	}
}

void
fe_ignore_update (int level)
{
	(void)level;
	set_status (_("Ignore list updated"));
}

void
fe_beep (session *sess)
{
	(void)sess;
	fl_beep ();
}

void
fe_lastlog (session *sess, session *lastlog_sess, char *sstr, gtk_xtext_search_flags flags)
{
	(void)sess; (void)lastlog_sess; (void)flags;
	append_text (sess, sstr ? sstr : _("Lastlog"));
}

void
fe_set_lag (server *serv, long lag)
{
	(void)serv;
	char buf[64];
	if (lag >= 0)
		g_snprintf (buf, sizeof buf, _("Lag: %ld ms"), lag);
	else
		g_snprintf (buf, sizeof buf, _("Lag: ?"));

	if (fltk_debug)
		debug_log ("fe_set_lag lag=%ld", lag);

	if (lag_indicator)
	{
		long val = lag;
		if (val < 0) val = 0;
		if (val > 1000) val = 1000;
		lag_indicator->value (val);
		lag_indicator->copy_label ("");
	}
}

void
fe_set_throttle (server *serv)
{
	if (!throttle_indicator)
		return;

	if (serv && serv->outbound_queue)
	{
		throttle_indicator->value (1);
		throttle_indicator->copy_label ("");
	}
	else
	{
		throttle_indicator->value (0);
		throttle_indicator->copy_label ("");
	}
}

void
fe_set_away (server *serv)
{
	debug_log ("fe_set_away is_away=%d", serv ? serv->is_away : -1);
	if (serv && serv->is_away)
		set_status (_("Away"));
	else
		set_status (_("Ready"));
}

void
fe_serverlist_open (session *sess)
{
	servlist_open (sess);
}

void
fe_get_bool (char *title, char *prompt, void *callback, void *userdata)
{
	int choice = fl_choice ("%s", _("No"), _("Yes"), NULL, prompt ? prompt : (title ? title : _("Question")));
	if (callback && choice == 1)
		((void (*)(void *))callback)(userdata);
}

void
fe_get_str (char *prompt, char *def, void *callback, void *ud)
{
	const char *res = fl_input ("%s", def ? def : "", prompt ? prompt : _("Input"));
	if (callback)
	{
		char *copy = res ? g_strdup (res) : NULL;
		((void (*)(void *, char *))callback)(ud, copy);
	}
}

void
fe_get_int (char *prompt, int def, void *callback, void *ud)
{
	char defbuf[32];
	g_snprintf (defbuf, sizeof defbuf, "%d", def);
	const char *res = fl_input ("%s", defbuf, prompt ? prompt : _("Input number"));
	if (callback)
	{
		int val = res ? atoi (res) : def;
		((void (*)(void *, int))callback)(ud, val);
	}
}

void
fe_get_file (const char *title, char *initial,
			 void (*callback) (void *userdata, char *file), void *userdata,
			 int flags)
{
	(void)flags;
	const char *path = fl_file_chooser (title ? title : _("Select File"), nullptr, initial);
	if (callback)
	{
		char *copy = path ? g_strdup (path) : NULL;
		callback (userdata, copy);
	}
}

void
fe_ctrl_gui (session *sess, fe_gui_action action, int arg)
{
	(void)sess; (void)arg;
	if (!main_win)
		return;
	switch (action)
	{
	case FE_GUI_SHOW: main_win->show (); break;
	case FE_GUI_HIDE: main_win->hide (); break;
	case FE_GUI_FOCUS: main_win->take_focus (); break;
	case FE_GUI_ICONIFY: main_win->iconize (); break;
	default: break;
	}
}

int
fe_gui_info (session *sess, int info_type)
{
	(void)sess;
	if (!main_win)
		return 0;
	switch (info_type)
	{
	case 0: return main_win->visible ();
	case 1: return main_win->shown ();
	default: return 0;
	}
}

void *
fe_gui_info_ptr (session *sess, int info_type)
{
	(void)sess;
	if (info_type == 0 || info_type == 1)
		return main_win;
	return NULL;
}

void
fe_confirm (const char *message, void (*yesproc)(void *), void (*noproc)(void *), void *ud)
{
	int choice = fl_choice ("%s", _("No"), _("Yes"), NULL, message ? message : _("Confirm"));
	if (choice == 1 && yesproc)
		yesproc (ud);
	else if (choice == 0 && noproc)
		noproc (ud);
}

char *
fe_get_inputbox_contents (struct session *sess)
{
	(void)sess;
	if (!input_box)
		return g_strdup ("");
	return g_strdup (input_box->value ());
}

int
fe_get_inputbox_cursor (struct session *sess)
{
	(void)sess;
	if (!input_box)
		return 0;
	return input_box->position ();
}

void
fe_set_inputbox_contents (struct session *sess, char *text)
{
	(void)sess;
	if (input_box)
		input_box->value (text ? text : "");
}

void
fe_set_inputbox_cursor (struct session *sess, int delta, int pos)
{
	(void)sess; (void)delta;
	if (input_box)
		input_box->position (pos);
}

void
fe_open_url (const char *url)
{
	if (!url)
		return;
	g_app_info_launch_default_for_uri (url, NULL, NULL);
}

// Dynamic menu callback for user-added menu items
static void
dynamic_menu_cb (Fl_Widget *, void *data)
{
	MenuEntry *me = static_cast<MenuEntry *>(data);
	if (me && !me->cmd.empty () && current_sess)
		handle_command (current_sess, (char *)me->cmd.c_str (), FALSE);
}

void
fe_menu_del (menu_entry *me)
{
	if (!me || !me->path)
		return;

	// Find and remove from our list
	for (auto it = dynamic_menus.begin (); it != dynamic_menus.end (); ++it)
	{
		if (it->path == me->path)
		{
			// Remove from FLTK menu bar
			if (menu_bar)
			{
				int idx = menu_bar->find_index (me->path);
				if (idx >= 0)
					menu_bar->remove (idx);
			}
			dynamic_menus.erase (it);
			break;
		}
	}
}

char *
fe_menu_add (menu_entry *me)
{
	if (!me || !me->path || !menu_bar)
		return g_strdup ("");

	// Store menu entry
	MenuEntry entry;
	entry.path = me->path;
	entry.label = me->label ? me->label : "";
	entry.cmd = me->cmd ? me->cmd : "";
	entry.pos = me->pos;
	entry.is_main = me->is_main;
	entry.enabled = me->enable;
	dynamic_menus.push_back (entry);

	// Add to FLTK menu bar
	int flags = 0;
	if (!me->enable)
		flags |= FL_MENU_INACTIVE;

	menu_bar->add (me->path, 0, dynamic_menu_cb, &dynamic_menus.back (), flags);

	return g_strdup (me->path);
}

void
fe_menu_update (menu_entry *me)
{
	if (!me || !me->path || !menu_bar)
		return;

	// Find in menu bar and update
	const Fl_Menu_Item *item = menu_bar->find_item (me->path);
	if (item)
	{
		Fl_Menu_Item *mutable_item = const_cast<Fl_Menu_Item *>(item);
		if (me->enable)
			mutable_item->activate ();
		else
			mutable_item->deactivate ();
	}
}

void
fe_server_event (server *serv, int type, int arg)
{
	(void)arg;
	const char *msg = nullptr;
	debug_log ("fe_server_event type=%d", type);
	switch (type)
	{
	case FE_SE_CONNECT:
		msg = _("Connecting...");
		break;
	case FE_SE_LOGGEDIN:
		msg = _("Logged in");
		break;
	case FE_SE_DISCONNECT:
		msg = _("Disconnected");
		joind_server_cleanup (serv);
		break;
	case FE_SE_RECONDELAY:
		msg = _("Reconnecting...");
		break;
	case FE_SE_CONNECTING:
		msg = _("Connecting...");
		break;
	default:
		return;
	}
	set_status (msg);
}

void
fe_tray_set_flash (const char *filename1, const char *filename2, int timeout)
{
	(void)filename1; (void)filename2; (void)timeout;
	// Tray features intentionally disabled for FLTK build
}

void
fe_tray_set_file (const char *filename)
{
	(void)filename;
}

void
fe_tray_set_icon (feicon icon)
{
	(void)icon;
}

void
fe_tray_set_tooltip (const char *text)
{
	(void)text;
}

void
fe_open_chan_list (server *serv, char *filter, int do_refresh)
{
	(void)filter;
	if (serv)
		chanlist_open (serv, do_refresh);
}

const char *
fe_get_default_font (void)
{
	return "Monospace 12";
}

} // extern "C"
