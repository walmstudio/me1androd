/* linux/i18n.h - interface languages of the Linux launcher / loading screen (v1.1). The source strings are Russian and
 * are the keys; translations: android/assets/i18n/<code>.json (shared with the Android launcher) + linux/i18n/<code>.json,
 * both embedded. Lookup: language -> English -> the Russian original (as android L.java). */
#pragma once
#define I18N_N 9
extern const char* const I18N_CODES[I18N_N];   /* ru en de fr it es pl cs hu */
extern const char* const I18N_NAMES[I18N_N];
extern const char* const I18N_GAME[I18N_N];    /* game.language values: RU INT DE FR IT ES PL CZ HU */
void i18n_set(int lang);                       /* index into I18N_CODES */
int  i18n_cur(void);
int  i18n_find(const char* code);              /* -1 if unknown */
int  i18n_system(void);                        /* $LC_ALL / $LC_MESSAGES / $LANG -> index (English if not ours) */
const char* T(const char* s);
char* i18n_tx(const char* s);                  /* native (C) message with Russian fragments -> malloc'd translation */
