/* RetroArch's playlists name games by path: when RommPS moves games to
 * another folder, the entries that point at them move too, so the history
 * and any playlist the user made keep working. */
#ifndef ROMM_PLAYLIST_H
#define ROMM_PLAYLIST_H

/* Paths under from move to under to in every playlist of the RetroArch at
 * root (playlist_dir, "" for <root>/playlists, and its builtin/ history). */
void playlists_move(const char *root, const char *playlist_dir, const char *from, const char *to);

#endif
