#ifndef ASMP_DIAG_PROFILE_H
#define ASMP_DIAG_PROFILE_H
#include "../../asmp-dll/src/game/steam_profile.h"
enum DiagStatus { DIAG_STARTING, DIAG_WAITING, DIAG_SAMPLING, DIAG_REJECTED, DIAG_ERROR, DIAG_STOPPED };
int hash_file_sha256(const wchar_t* path, char output[65]);
#endif
