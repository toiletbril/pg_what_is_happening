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

#ifndef PWH_BG_WORKER_H
#define PWH_BG_WORKER_H

#include "postgres.h"

#include "common.h"

extern void			   pwh_register_openmetrics_exporter_as_bg_worker(void);
extern wontreturn void pwh_bgworker_main(Datum main_arg);

#endif /* PWH_BG_WORKER_H */
