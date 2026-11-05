// A hot reload session: the watcher thread, from the first build of an app's sources on.
#pragma once

#include "build.h"

typedef struct ui_dev_session ui_dev_session;

// Starts watching desc->root, builds every source and posts the result, then rebuilds what
// changes. `desc`'s strings must outlive the session. NULL: the root can't be watched.
ui_dev_session *ui_dev_session_start(const ui_dev_build_desc *desc, ui_dev_channel *channel);
void ui_dev_session_stop(ui_dev_session *s);
