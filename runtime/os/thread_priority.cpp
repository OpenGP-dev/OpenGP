// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/thread_priority.h"

#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace os {

void set_interactive_thread() {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

}  // namespace os
