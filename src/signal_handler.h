/*
 * This file is part of pg_what_is_happening.
 * Copyright (C) 2025 toilebril
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 * See top-level LICENSE file.
 */

#ifndef PWH_SIGNAL_HANDLER_H
#define PWH_SIGNAL_HANDLER_H

#include "postgres.h"

#include "common.h"
#include "executor/execdesc.h"

/* Set the current QueryDesc for signal handler access. */
extern void pwh_set_current_query_desc(QueryDesc *queryDesc);

/* Get the current QueryDesc. */
extern QueryDesc *pwh_get_current_query_desc(void);

/* SIGUSR2 signal handler. */
extern void pwh_sigusr2_handler(SIGNAL_ARGS);

/* Install signal handler. */
extern void pwh_install_signal_handler(void);

extern u64 pwh_get_signal_handler_call_count(void);
extern u64 pwh_get_signal_handler_success_count(void);
extern u64 pwh_get_signal_handler_no_querydesc(void);
extern u64 pwh_get_signal_handler_shmem_null(void);
extern u64 pwh_get_signal_handler_no_slot(void);

#endif /* PWH_SIGNAL_HANDLER_H. */
