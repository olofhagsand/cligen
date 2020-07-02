/*
  CLIgen application reading CLI specification from file

  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2001-2020 Olof Hagsand

  This file is part of CLIgen.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 2 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>

#include <cligen/cligen.h>

/*! General callback for executing shells. 
 * The argument is a command followed by arguments as defined in the input syntax.
 * Simple example:
 *   CLIgen input syntax:     <a type:int32>, cligen_exec_cb("ls ${a}");
 *   CLI input:               > 42
 *   Shell command:           ls 42
 * More advanced example:
 *   CLIgen input syntax:     [<a type:int> | <b type:ipv4addr>], 
 *                                cligen_exec_cb("foo.sh ${a:-99} ${b:-1.2.3.4}");
 *   CLI input:               > 22
 *   Shell command:           foo.sh 22 1.2.3.4
 *   CLI input:               > 2.3.4.5
 *   Shell command:           foo.sh 99 1.2.3.4.
 */
int
cligen_exec_cb(cligen_handle handle, cvec *cvv, cvec *argv)
{
    cg_var *cv = NULL;
    char    buf[64];
    int     pid;
    int     ret;
    int     status;

    if (argv == NULL)
	return 0;
    if ((pid = fork()) == 0){ /* child */
	while ((cv = cvec_each1(cvv, cv)) != NULL) {
	    if (cv_const_get(cv))
		continue;
	    cv2str(cv, buf, sizeof(buf)-1);
	    setenv(cv_name_get(cv), buf, 1 );
	}
	cv2str(cvec_i(argv, 0), buf, sizeof(buf)-1);
	ret = system(buf);
	exit(0);
    }
    /* Wait for child to finish */
    if(waitpid (pid, &status, 0) == pid)
	ret = WEXITSTATUS(status);
    else
	ret = -1;
    return ret;
}

/*! CLI generic callback printing the variable vector and argument
 */
int
callback(cligen_handle handle, cvec *cvv, cvec *argv)
{
    int     i = 1;
    cg_var *cv;
    char    buf[64];

    fprintf(stderr, "function: %s\n", cligen_fn_str_get(handle));
    fprintf(stderr, "variables:\n");
    cv = NULL;
    while ((cv = cvec_each1(cvv, cv)) != NULL) {
	cv2str(cv, buf, sizeof(buf)-1);
	fprintf(stderr, "\t%d name:%s type:%s value:%s\n", 
		i++, 
		cv_name_get(cv),
		cv_type2str(cv_type_get(cv)),
		buf
	    );
    }
    if (argv){
	    cv = NULL;
	    i=0;
	    while ((cv = cvec_each(argv, cv)) != NULL) {
		cv2str(cv, buf, sizeof(buf)-1);
		fprintf(stderr, "arg %d: %s\n", i++, buf);
	    }
	}
    return 0;
}

/*! Example of static string to function mapper
 * Note, the syntax need to something like: "a{help}, callback(42)"
 */
cgv_fnstype_t *
str2fn(char *name, void *arg, char **error)
{
    *error = NULL;
    if (strcmp(name, "callback") == 0)
	return callback;
    if (strcmp(name, "cligen_exec_cb") == 0)
	return cligen_exec_cb;
    return callback; /* allow any function (for testing) */
}

/*! Example of expansion(completion) function. 
 * It is called every time a variable of the form <expand> needs to be evaluated.
 * Note the mallocing of vectors which could probably be done in a
 * friendlier way.
 * Note also that the expansion is not very dynamic, a script or reading a file
 * would have introduced som more dynamics.
 */
static int
cli_expand_cb(cligen_handle h, 
	      char         *fn_str, 
	      cvec         *cvv, 
	      cvec         *argv, 
	      cvec         *commands,     /* vector of function strings */
	      cvec         *helptexts)   /* vector of help-texts */
{
#if 1
    /* Special case for two partly overlapping expand sets */
    if (strcmp(fn_str,"exp")==0){
	cvec_add_string(commands, NULL, (char*)"exp1"); cvec_add_string(helptexts, NULL, (char*)"Help exp1");
	cvec_add_string(commands, NULL, (char*)"exp2"); cvec_add_string(helptexts, NULL, (char*)"Help exp2");
	cvec_add_string(commands, NULL, (char*)"exp3"); cvec_add_string(helptexts, NULL, (char*)"Help exp3");
    }
    else{
	cvec_add_string(commands, NULL, (char*)"exp2");  cvec_add_string(helptexts, NULL, (char*)"Help exp2");
    }
#else
    cvec_add_string(commands, NULL, (char*)"auto");
    cvec_add_string(helptexts, NULL, (char*)"This expansion is auto-generated by -e option");
#endif
    return 0;
}

/*! Trivial function translator/mapping function that just assigns same callback
 */
static expandv_cb *
str2fn_exp(char *name, void *arg, char **error)
{
    return cli_expand_cb;
}

/*
 * Global variables.
 */
static void 
usage(char *argv)
{
    fprintf(stderr, "Usage:%s [-h][-f <filename>][-1][-p][-P], where the optoions have the following meaning:\n"
	    "\t-h \t\tHelp\n"
	    "\t-f <file> \tConfig-file (or stdin)\n"
	    "\t-1 \t\tOnce only. Do not enter interactive mode\n"
	    "\t-p \t\tPrint syntax\n"
	    "\t-e \t\tSet automatic expansion/completion for all expand() functions\n"
	    "\t-P \t\tSet preference mode to 1, ie return first if several have same pref\n"
	    ,
	    argv);
    exit(0);
}

/* Main */
int
main(int argc, char *argv[])
{
    int         retval = -1;
    parse_tree *pt = NULL;
    FILE       *f = stdin;
    char       *argv0 = argv[0];
    char       *filename=NULL;
    cvec       *globals;   /* global variables from syntax */
    cligen_handle  h;
    char       *str;
    int         once = 0;
    int         print_syntax = 0;
    int         set_expand = 0;
    int         set_preference = 0;

    argv++;argc--;
    for (;(argc>0)&& *argv; argc--, argv++){
	if (**argv != '-')
	    break;
	(*argv)++;
	if (strlen(*argv)==0)
	    usage(argv0);
	switch(**argv) {
	case 'h': /* help */
	    usage(argv0); /* usage exits */
	    break;
	case '1': /* quit directly */
	    once++;
	    break;
	case 'p': /* print syntax */
	    print_syntax++;
	    break;
	case 'e': /* Set automatic completion/expand */
	    set_expand++;
	    break;
	case 'P': /* Return first if several have same preference */
	    set_preference++;
	    break;
	case 'f' : 
	    argc--;argv++;
	    filename = *argv;
	    if ((f = fopen(filename, "r")) == NULL){
		fprintf(stderr, "fopen(%s): %s\n", filename, strerror(errno));
		exit(1);
	    }
	    break;
	default:
	    usage(argv0);
	    break;
	}
  }
    if ((h = cligen_init()) == NULL)
	goto done;    
    cligen_lexicalorder_set(h, 1);
    cligen_ignorecase_set(h, 1);
    if (set_preference)
	cligen_preference_mode_set(h, set_preference);
//    cligen_parse_debug(1);
    if ((globals = cvec_new(0)) == NULL)
	goto done;
    if (cligen_parse_file(h, f, filename?filename:(char*)"stdin", NULL, globals) < 0)
	goto done;

    pt = cligen_tree_i(h, 0); 

    /* map functions */
    if (pt) {
	if (cligen_callbackv_str2fn(pt, str2fn, NULL) < 0)   /* callback */
	    goto done;
	if (set_expand &&
	    cligen_expandv_str2fn(pt, str2fn_exp, NULL) < 0) /* expand */
	    goto done;
    }
    if ((str = cvec_find_str(globals, (char*)"prompt")) != NULL)
	cligen_prompt_set(h, str);
    if ((str = cvec_find_str(globals, (char*)"tabmode")) != NULL)
	if (strcmp(str,(char*)"long") == 0)
	    cligen_tabmode_set(h, CLIGEN_TABMODE_COLUMNS);
    if ((str = cvec_find_str(globals, (char*)"comment")) != NULL)
	cligen_comment_set(h, *str);
    if ((str = cvec_find_str(globals, (char*)"mode")) != NULL)
	cligen_tree_active_set(h, str);
    cvec_free(globals);

    if (print_syntax){
	pt_print(stdout, pt, 0);
	fflush(stdout);
    }
    if (once)
	goto done;
    if (cligen_loop(h) < 0)
	goto done;
    retval = 0;
  done:
    fclose(f);
    if (h)
	cligen_exit(h);
    return retval;
}
