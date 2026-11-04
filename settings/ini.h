/* Line-preserving INI reader/writer compatible with Unreal Engine 3 / Win32 profile files.
 * Keeps comments, ordering, duplicate keys and the original encoding (ANSI/UTF-8 or UTF-16LE with BOM). */
#pragma once
#include <stddef.h>
typedef struct Ini {
    char** lines; int n, cap;   /* UTF-8 text, no line terminators */
    int utf16, bom8, crlf, noeol;   /* original encoding / line endings / last line without terminator */
} Ini;
void  ini_init(Ini* ini);
void  ini_free(Ini* ini);
int   ini_load_file(Ini* ini, const char* path);          /* 0 ok, -1 cannot open */
int   ini_load_mem(Ini* ini, const void* data, size_t n);
int   ini_save_file(const Ini* ini, const char* path);    /* 0 ok */
char* ini_serialize(const Ini* ini, size_t* n);           /* malloc'd bytes in the original encoding */
/* value lookup: section/key case-insensitive, first match; returns 1 if found */
int   ini_get(const Ini* ini, const char* sec, const char* key, char* out, size_t cap);
void  ini_set(Ini* ini, const char* sec, const char* key, const char* val);   /* replace first, drop later duplicates, or append */
int   ini_remove_key(Ini* ini, const char* sec, const char* key);
int   ini_remove_section(Ini* ini, const char* sec);
int   ini_has_section(const Ini* ini, const char* sec);
/* enumerate: callback per section name / per key of a section */
void  ini_sections(const Ini* ini, void (*cb)(const char* name, void* u), void* u);
void  ini_keys(const Ini* ini, const char* sec, void (*cb)(const char* key, const char* val, void* u), void* u);
/* numeric helpers for UE3 style values ("TRUE", "1;", "4.5 // comment") */
int   ini_parse_bool(const char* v, int def);
