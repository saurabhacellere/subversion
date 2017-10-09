/*
 * shelve-cmd.c -- Shelve commands.
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include "svn_client.h"
#include "svn_error_codes.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_utf.h"

#include "cl.h"

#include "svn_private_config.h"
#include "private/svn_sorts_private.h"


/* First argument should be the name of a shelved change. */
static svn_error_t *
get_name(const char **name,
         apr_getopt_t *os,
         apr_pool_t *result_pool,
         apr_pool_t *scratch_pool)
{
  apr_array_header_t *args;

  SVN_ERR(svn_opt_parse_num_args(&args, os, 1, scratch_pool));
  SVN_ERR(svn_utf_cstring_to_utf8(name,
                                  APR_ARRAY_IDX(args, 0, const char *),
                                  result_pool));
  return SVN_NO_ERROR;
}

/*  */
static int
compare_dirents_by_mtime(const svn_sort__item_t *a,
                         const svn_sort__item_t *b)
{
  svn_client_shelved_patch_info_t *a_val = a->value;
  svn_client_shelved_patch_info_t *b_val = b->value;

  return (a_val->dirent->mtime < b_val->dirent->mtime)
           ? -1 : (a_val->dirent->mtime > b_val->dirent->mtime) ? 1 : 0;
}

/* Return a list of shelved changes sorted by patch file mtime, oldest first.
 */
static svn_error_t *
list_sorted_by_date(apr_array_header_t **list,
                    const char *local_abspath,
                    svn_client_ctx_t *ctx,
                    apr_pool_t *scratch_pool)
{
  apr_hash_t *shelved_patch_infos;

  SVN_ERR(svn_client_shelves_list(&shelved_patch_infos, local_abspath,
                                  ctx, scratch_pool, scratch_pool));
  *list = svn_sort__hash(shelved_patch_infos, compare_dirents_by_mtime,
                         scratch_pool);
  return SVN_NO_ERROR;
}

/* Display a list of shelved changes */
static svn_error_t *
shelves_list(const char *local_abspath,
             svn_boolean_t diffstat,
             svn_client_ctx_t *ctx,
             apr_pool_t *scratch_pool)
{
  apr_array_header_t *list;
  int i;

  SVN_ERR(list_sorted_by_date(&list,
                              local_abspath, ctx, scratch_pool));

  for (i = 0; i < list->nelts; i++)
    {
      const svn_sort__item_t *item = &APR_ARRAY_IDX(list, i, svn_sort__item_t);
      const char *name = item->key;
      svn_client_shelved_patch_info_t *info = item->value;
      int age = (apr_time_now() - info->mtime) / 1000000 / 60;

      printf("%-30s %6d mins old %10ld bytes\n",
             name, age, (long)info->dirent->filesize);
      printf(" %.50s\n",
             info->message);

      if (diffstat)
        {
          system(apr_psprintf(scratch_pool, "diffstat %s 2> /dev/null",
                              info->patch_path));
          printf("\n");
        }
    }

  return SVN_NO_ERROR;
}

/* Find the name of the youngest shelved change.
 */
static svn_error_t *
name_of_youngest(const char **name_p,
                 const char *local_abspath,
                 svn_client_ctx_t *ctx,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool)
{
  apr_hash_t *dirents;
  apr_array_header_t *list;
  const svn_sort__item_t *youngest_item;

  SVN_ERR(svn_client_shelves_list(&dirents, local_abspath,
                                  ctx, scratch_pool, scratch_pool));
  if (apr_hash_count(dirents) == 0)
    return svn_error_create(SVN_ERR_CL_INSUFFICIENT_ARGS, NULL,
                            _("No shelved changes found"));

  list = svn_sort__hash(dirents, compare_dirents_by_mtime, scratch_pool);
  youngest_item = &APR_ARRAY_IDX(list, list->nelts - 1, svn_sort__item_t);
  *name_p = apr_pstrndup(result_pool, youngest_item->key,
                         strlen(youngest_item->key) - 6 /* remove '.patch' */);
  return SVN_NO_ERROR;
}

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__shelve(apr_getopt_t *os,
               void *baton,
               apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  const char *local_abspath;
  const char *name;
  apr_array_header_t *targets;

  if (opt_state->quiet)
    ctx->notify_func2 = NULL; /* Easy out: avoid unneeded work */

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, "", pool));

  if (opt_state->list)
    {
      if (os->ind < os->argc)
        return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, 0, NULL);

      SVN_ERR(shelves_list(local_abspath,
                           ! opt_state->quiet /*diffstat*/,
                           ctx, pool));
      return SVN_NO_ERROR;
    }

  SVN_ERR(get_name(&name, os, pool, pool));

  if (opt_state->remove)
    {
      if (os->ind < os->argc)
        return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, 0, NULL);

      SVN_ERR(svn_client_shelves_delete(name, local_abspath,
                                        opt_state->dry_run,
                                        ctx, pool));
      if (! opt_state->quiet)
        SVN_ERR(svn_cmdline_printf(pool, "deleted '%s'\n", name));
      return SVN_NO_ERROR;
    }

  /* Parse the remaining arguments as paths. */
  SVN_ERR(svn_cl__args_to_target_array_print_reserved(&targets, os,
                                                      opt_state->targets,
                                                      ctx, FALSE, pool));
  svn_opt_push_implicit_dot_target(targets, pool);

  {
      svn_depth_t depth = opt_state->depth;
      svn_error_t *err;

      /* shelve has no implicit dot-target `.', so don't you put that
         code here! */
      if (!targets->nelts)
        return svn_error_create(SVN_ERR_CL_INSUFFICIENT_ARGS, 0, NULL);

      SVN_ERR(svn_cl__check_targets_are_local_paths(targets));

      if (depth == svn_depth_unknown)
        depth = svn_depth_infinity;

      SVN_ERR(svn_cl__eat_peg_revisions(&targets, targets, pool));

      if (ctx->log_msg_func3)
        SVN_ERR(svn_cl__make_log_msg_baton(&ctx->log_msg_baton3,
                                           opt_state, NULL, ctx->config,
                                           pool));
      err = svn_client_shelve(name,
                              targets, depth, opt_state->changelists,
                              opt_state->keep_local, opt_state->dry_run,
                              ctx, pool);
      if (ctx->log_msg_func3)
        SVN_ERR(svn_cl__cleanup_log_msg(ctx->log_msg_baton3,
                                        err, pool));
      else
        SVN_ERR(err);

      if (! opt_state->quiet)
        SVN_ERR(svn_cmdline_printf(pool, "shelved '%s'\n", name));
  }

  return SVN_NO_ERROR;
}

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__unshelve(apr_getopt_t *os,
                 void *baton,
                 apr_pool_t *pool)
{
  svn_cl__opt_state_t *opt_state = ((svn_cl__cmd_baton_t *) baton)->opt_state;
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  const char *local_abspath;
  const char *name;
  apr_array_header_t *targets;

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, "", pool));

  if (opt_state->list)
    {
      if (os->ind < os->argc)
        return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, 0, NULL);

      SVN_ERR(shelves_list(local_abspath,
                           ! opt_state->quiet /*diffstat*/,
                           ctx, pool));
      return SVN_NO_ERROR;
    }

  if (os->ind < os->argc)
    {
      SVN_ERR(get_name(&name, os, pool, pool));
    }
  else
    {
      SVN_ERR(name_of_youngest(&name, local_abspath, ctx, pool, pool));
      printf("unshelving the youngest change, '%s'\n", name);
    }

  /* There should be no remaining arguments. */
  SVN_ERR(svn_cl__args_to_target_array_print_reserved(&targets, os,
                                                      opt_state->targets,
                                                      ctx, FALSE, pool));
  if (targets->nelts)
    return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, 0, NULL);

  if (opt_state->quiet)
    ctx->notify_func2 = NULL; /* Easy out: avoid unneeded work */

  SVN_ERR(svn_client_unshelve(name, local_abspath,
                              opt_state->keep_local, opt_state->dry_run,
                              ctx, pool));
  if (! opt_state->quiet)
    SVN_ERR(svn_cmdline_printf(pool, "unshelved '%s'\n", name));

  return SVN_NO_ERROR;
}

/* This implements the `svn_opt_subcommand_t' interface. */
svn_error_t *
svn_cl__shelves(apr_getopt_t *os,
                void *baton,
                apr_pool_t *pool)
{
  svn_client_ctx_t *ctx = ((svn_cl__cmd_baton_t *) baton)->ctx;
  const char *local_abspath;

  /* There should be no remaining arguments. */
  if (os->ind < os->argc)
    return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, 0, NULL);

  SVN_ERR(svn_dirent_get_absolute(&local_abspath, "", pool));
  SVN_ERR(shelves_list(local_abspath, TRUE /*diffstat*/, ctx, pool));

  return SVN_NO_ERROR;
}
