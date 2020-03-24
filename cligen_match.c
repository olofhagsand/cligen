/*
  CLI generator match functions, used in runtime checks.

  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2001-2019 Olof Hagsand

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
#include "cligen_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <assert.h>

#define __USE_GNU /* isblank() */
#include <ctype.h>
#ifndef isblank
#define isblank(c) (c==' ')
#endif /* isblank */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "cligen_buf.h"
#include "cligen_cv.h"
#include "cligen_cvec.h"
#include "cligen_gen.h"
#include "cligen_handle.h"
#include "cligen_expand.h"
#include "cligen_match.h"

#ifndef MIN
#define MIN(x,y) ((x)<(y)?(x):(y))
#endif

#define ISREST(co) ((co)->co_type == CO_VARIABLE && (co)->co_vtype == CGV_REST)

/*! Match variable against input string
 * 
 * @param[in]  string  Input string to match
 * @param[in]  pvt     variable type (from definition)
 * @param[in]  cmd     variable string (from definition) - can contain range
 * 
 * @retval     -1      Error (print msg on stderr)
 * @retval     0       Not match and reason returned as malloced string.
 * @retval     1       Match
 * Who prints errors?
 * @see cvec_match where actual allocation of variables is made not only sanity
 */
static int
match_variable(cligen_handle h,
	       cg_obj       *co, 
	       char         *str, 
	       char        **reason)
{
    int         retval = -1;
    cg_var     *cv; /* Just a temporary cv for validation */
    cg_varspec *cs;

    cs = &co->u.cou_var;
    if ((cv = cv_new(co->co_vtype)) == NULL)
	goto done;
    if (co->co_vtype == CGV_DEC64) /* XXX: Seems misplaced? / too specific */
	cv_dec64_n_set(cv, cs->cgs_dec64_n);
    if ((retval = cv_parse1(str, cv, reason)) <= 0) 
	goto done;
    /* here retval should be 1 */
    /* Validate value */
    if ((retval = cv_validate(h, cv, cs, reason)) <= 0)
	goto done;
    /* here retval should be 1 */
  done:
    if (cv)
	cv_free(cv);
    return retval; 
}

/*! Given a string and one cligen object, return if the string matches
 * @param[in]  string  Input string to match (NULL is match)
 * @param[in]  co      cligen object
 * @param[out] exact   1 if match is exact (CO_COMMANDS). VARS is 0.
 * @param[out] reason  if not match and co type is 0, reason points to a (malloced) 
 *                     string containing an error explanation string. If reason is
 *                     NULL no such string will be malloced. This string needs to
 *                     be freed.
 * @retval  -1         Error
 * @retval   0         Not match
 * @retval   1         Match
 */
static int 
match_object(cligen_handle h,
	     char         *string,
	     cg_obj       *co, 
	     int          *exact,
	     char        **reason)
{
  int match = 0;

  *exact = 0;
  if (co==NULL)
      return 0;
  switch (co->co_type){
  case CO_COMMAND:
      if (string == NULL)
	  match++;
      else{
	  match = (strncmp(co->co_command, string, strlen(string)) == 0);
	  *exact = strlen(co->co_command) == strlen(string);
      }
    break;
  case CO_VARIABLE:
      if (string == NULL || strlen(string)==0)
	  match++;
      else
	  if ((match = match_variable(h, co, string, reason)) < 0)
	      return -1;
    break;
  case CO_REFERENCE:
      break;
  }
  return match!=0 ? 1 : 0;
}

/*! Check if "perfect" match, ie a command and matches whole command
 */
static int
match_perfect(char   *string, 
	      cg_obj *co)
{
  return  ((co->co_type==CO_COMMAND) &&
	   (strcmp(string, co->co_command)==0));
}

/*! Given a string (s0), return the next token. 
 * The string is modified to return
 * the remainder of the string after the identified token.
 * A token is found either as characters delimited by one or many delimiters.
 * Or as a pair of double-quotes(") with any characters in between.
 * if there are trailing spaces after the token, trail is set to one.
 * If string is NULL or "", NULL is returned.
 * If empty token found, s0 is NULL
 * @param[in]  s0       String, the string is modified like strtok
 * @param[out] token0   A malloced token.  NOTE: token must be freed after use.
 * @param[out] rest0    A remaining (rest) string.  NOTE: NOT malloced.
 * @param[out] leading0 If leading delimiters eg " thisisatoken"
 * Example:
 *   s0 = "  foo bar"
 * results in token="foo", leading=1
 */
static int
next_token(char **s0, 
	   char **token0,
	   char **rest0, 
	   int   *leading0)
{
    char  *s;
    char  *st;
    char  *token = NULL;
    size_t len;
    int    quote=0;
    int    leading=0;
    int    escape = 0;

    s = *s0;
    if (s==NULL){
	fprintf(stderr, "%s: null string\n", __FUNCTION__);
	return -1;
    }
    for (s=*s0; *s; s++){ /* First iterate through delimiters */
	if (index(CLIGEN_DELIMITERS, *s) == NULL)
	    break;
	leading++;
    }
    if (rest0)
	*rest0 = s;
    if (*s && index(CLIGEN_QUOTES, *s) != NULL){
	quote++;
	s++;
    }
    st=s; /* token starts */
    escape = 0;
    for (; *s; s++){ /* Then find token */
	if (quote){
	    if (index(CLIGEN_QUOTES, *s) != NULL)
		break;
	}
	else{ /* backspace tokens for escaping delimiters */
	    if (escape)
		escape = 0;
	    else{
		if (*s == '\\')
		    escape++;
		else
		    if (index(CLIGEN_DELIMITERS, *s) != NULL)
			break;
	    }
	}
    }
    if (quote && *s){
	s++;
	// fprintf(stderr, "s=\"%s\" %d %s\n", s, *s, index(CLIGEN_DELIMITERS, *s));
	if (*s && index(CLIGEN_DELIMITERS, *s) == NULL){
	    ;//	cligen_reason("Quote token error");
	}
	len = (s-st)-1;
    }
    else{
	if (quote){ /* Here we signalled error before but it is removed */
	    st--;
	}
	len = (s-st);
	if (!len){
	    token = NULL;
	    *s0 = NULL;
	    goto done;
	}
    }
    if ((token=malloc(len+1)) == NULL){
	fprintf(stderr, "%s: malloc: %s\n", __FUNCTION__, strerror(errno));
	return -1;
    }
    memcpy(token, st, len);
    token[len] = '\0';
    *s0 = s;
 done:
    *leading0 = leading;
    *token0 = token;
    return 0;
}

/*! Split a CLIgen command string into a cligen variable vector using delimeters and escape quotes
 *
 * @param[in]  string String to split
 * @param[out] cvtp   CLIgen variable vector, containing all tokens. 
 * @param[out] cvrp   CLIgen variable vector, containing the remaining strings. 
 * @retval     0      OK
 * @retval    -1      Error
 * @code
 *   cvec  *cvt = NULL;
 *   cvec  *cvr = NULL;
 *   if (cligen_str2cvv("a=b&c=d", " \t", "\"", &cvt, &cvt) < 0)
 *     err;
 *   ...
 *   cvec_free(cvt);
 *   cvec_free(cvr);
 * @endcode
 * Example, input string "aa bb cc" (0th element is always whole string)
 *   cvp : ["aa bb cc", "aa", "bb", "cc"]
 *   cvr : ["aa bb cc", "aa bb cc", "bb cc", "cc"]
 * @note both out cvv:s should be freed with cvec_free()
 */
int
cligen_str2cvv(char  *string, 
	       cvec **cvtp,
    	       cvec **cvrp)
{
    int     retval = -1;
    char   *s;
    char   *sr;
    char   *s0 = NULL;;
    cvec   *cvt = NULL; /* token vector */
    cvec   *cvr = NULL; /* rest vector */
    cg_var *cv;
    char   *t;
    int     trail;
    int     i;

    if ((s0 = strdup(string)) == NULL)
	goto done;
    s = s0;
    if ((cvt = cvec_start(string)) ==NULL)
	goto done;
    if ((cvr = cvec_start(string)) ==NULL)
	goto done;
    i = 0;
    while (s != NULL) {
	if (next_token(&s, &t, &sr, &trail) < 0)
	    goto done;
	/* If there is no token, stop, 
	 * unless it is the intial token (empty string) OR there are trailing whitespace
	 * In these cases insert an empty "" token.
	 */
	if (t == NULL && !trail && i > 0)
	    break;
	if ((cv = cvec_add(cvr, CGV_STRING)) == NULL)
	    goto done;
	if (cv_string_set(cv, sr?sr:"") == NULL)
	    goto done;
	if ((cv = cvec_add(cvt, CGV_STRING)) == NULL)
	    goto done;
	if (cv_string_set(cv, t?t:"") == NULL)
	    goto done;
	if (t)
	    free(t);
	i++;
    }
    retval = 0;
    assert(cvec_len(cvt)>1); /* XXX */
    assert(cvec_len(cvr)>1); /* XXX */
    if (cvtp){
	*cvtp = cvt;
	cvt = NULL;
    }
    if (cvrp){
	*cvrp = cvr;
	cvr = NULL;
    }
 done:
    if (s0)
	free(s0);
    if (cvt)
	cvec_free(cvt);
    if (cvr)
	cvec_free(cvr);
    return retval;
}

/*! Returns the total number of "levels" of a CLIgen command string
 *
 * A level is an atomic command delimetered by space or tab.
 * Example: "", "a", "abcd" has level 0
 *          "abcd ", "vb fg" has level 1
 *          "abcd gh ", "vb fg hjsa" has level 2
 *
 * @param[in] cv    CLIgen variable vector, containing all tokens. 
 * @retval    0-n   Number of levels
 * @retval    -1    Error
 */
int
cligen_cvv_levels(cvec *cvv)
{
    size_t sz;
    
    if (cvv == NULL)
	return -1;
    sz = cvec_len(cvv);
    if (sz == 0)
	return -1;
    else return sz - 2;
}

/*! Return if the parse-tree is aonly variables, or if there is at least one non-variable
 * @param[in] pt  Parse-tree
 * @retval    0   Empty or contains at least one command (non-var) 
 * @retval    1   0 elements, only variables alternatives (or commands derived from variables)
 */
static int
pt_onlyvars(parse_tree pt)
{
    int     i;
    cg_obj *co;
    int     onlyvars = 0;
    
    for (i=0; i<pt.pt_len; i++){ 
	if ((co = pt.pt_vec[i]) == NULL)
	    continue;
	if (co->co_type != CO_VARIABLE && co->co_ref == NULL){
	    onlyvars = 0;
	    break;
	}
	onlyvars = 1;
    }
    return onlyvars;
}

/*! Help function to append a cv to a cvec. For expansion cvec passed to pt_expand_2
 * @param[in]  co     A cligen variable that has a matching value
 * @param[in]  cmd    Value in string of the variable
 * @param[out] cvv   The cligen variable vector to push a cv with name of co and
 *                    value in cmd
 * @retval     cv     Cligen variable
 * @retval     NULL   Error
 */
static cg_var *
add_cov_to_cvec(cg_obj *co, 
		char   *cmd, 
		cvec   *cvv)
{
    cg_var *cv = NULL;

    if ((cv = cvec_add(cvv, co->co_vtype)) == NULL)
	return NULL;
    cv_name_set(cv, co->co_command);
    cv_const_set(cv, iskeyword(co));
	if (co->co_vtype == CGV_DEC64) /* XXX: Seems misplaced? / too specific */
		cv_dec64_n_set(cv, co->co_dec64_n);
    if (cv_parse(cmd, cv) < 0) {
	cv_reset(cv);
	cvec_del(cvv, cv);
	return NULL;
    }
    return cv;
}

/*! Match terminal/leaf cligen objects. Multiple matches is used for completion.
 * We must have a preference when matching when it matches a command
 * and some variables.
 * The preference is:
 *  command > ipv4,mac > string > rest
 * return in matchvector which element match (their index)
 * and how many that match.
 * return value is the number of matches on return (also returned in matchlen)
 * index is the index of the first such match.
 * @param[in]  h        CLIgen handle
 * @param[in]  string0  Input string to match
 * @param[in]  cvt      Tokenized string: vector of tokens
 * @param[in]  cvr      Rest variant,  eg remaining string in each step
 * @param[in]  pt       Vector of commands (array of cligen object pointers (cg_obj)
 * @param[in]  pt_max   Length of the pt array
 * @param[in]  level    Current command level
 * @param[in]  levels   Total nr of command levels
 * @param[out] ptp      Returns the vector at the place of matching
 * @param[out] matchv   A vector of integers containing which 
 * @param[out] matchlen Length of matchv. That is, # of matches and same as return 
 *                      value (if 0-n)
 * @param[out] reason0  If retval = 0, this may be malloced to indicate reason for 
 *                      notmatching variables, if given. Neeed to be free:d
 * @retval     0-n      The number of matches in pt . See param matchlen.
 * @retval     -1       Error
 * @see match_pattern_node
 */
static int 
match_pattern_terminal(cligen_handle h, 
		       cvec         *cvt,
		       cvec         *cvr,
		       parse_tree    pt,
		       int           level, 
		       int           levels,
		       pt_vec       *ptp, 
		       int          *matchv[], 
		       int          *matchlen,
		       char        **reason0
		       )
{
    char   *str;
    int     i;
    int     match;
    int     matches = 0;
    cg_obj *co;
    cg_obj *co_match;
    cg_obj *co_orig;
    int     exact;
    char   *reason;
    int     onlyvars = 0; 

    co_match = NULL;
    /* If there are only variables in the list, then keep track of variable match errors 
     * This is logic to hinder error message to relate to variable mismatch
     * if there is a commands on same level with higher prio to match.
     * If all match fails, it is more interesting to understand the match fails
     * on commands, not variables.
     */
    onlyvars = (reason0==NULL)?0:pt_onlyvars(pt);

    for (i=0; i<pt.pt_len; i++){
	if ((co = pt.pt_vec[i]) == NULL)
	    continue;
	reason = NULL;
	/* Either individual token or rest-of-string */
	str = cvec_i_str(ISREST(co)?cvr:cvt, level+1);
	if ((match = match_object(h, str, co, &exact, onlyvars?&reason:NULL)) < 0)
	    goto error;
	if (match){ /* XXX DIFFERS from match_pattern_node */
	    assert(reason==NULL);
	    *matchlen = *matchlen + 1;
	    if ((*matchv = realloc(*matchv, (*matchlen)*sizeof(int))) == NULL){
		fprintf(stderr, "%s: realloc: %s\n", __FUNCTION__, strerror(errno));
		return -1;
	    }
	    co_match = co;
	    (*matchv)[matches++] = i; 
	}
	/* match == 0, co type is variable and onlyvars, then reason is set once
	 * this may not be the best preference, we just set the first
	 */
	if (reason){
	    if (*reason0 == NULL)
		*reason0 = reason;
	    reason = NULL;
	    onlyvars = 0;
	}
    } /* for pt.pt_len */
    if (matches){
	*ptp = pt.pt_vec;
	if (reason0 && *reason0){
	    free(*reason0);
	    *reason0 = NULL;
	}
	if (matches == 1){
	    assert(co_match);

	    co_orig = co_match->co_ref?co_match->co_ref: co_match;
	    if (co_match->co_type == CO_COMMAND && co_orig->co_type == CO_VARIABLE){
		if (co_value_set(co_orig, co_match->co_command) < 0)
		    goto error;
	    }
	    /* Cleanup made on top-level */
	}
    }
    *matchlen = matches;
 done:
    return matches;
  error:
    matches = -1;
    goto done;
}


/*! Match non-terminal cligen object. Need to match exact.
 *
 * @param[in]  h         CLIgen handle
 * @param[in]  string0   Input string to match
 * @param[in]  cvt      Tokenized string: vector of tokens
 * @param[in]  cvr      Rest variant,  eg remaining string in each step
 * @param[in]  pt        Vector of commands. Array of cligen object pointers
 * @param[in]  pt_max    Length of the pt array
 * @param[in]  level    Current command level
 * @param[in]  levels   Total nr of command levels
 * @param[in]  hide    Respect hide setting of commands (dont show)
 * @param[in]  expandvar Set if VARS should be expanded, eg ? <tab>
 * @param[out] ptp       Returns the vector at the place of matching
 * @param[out] matchv    A vector of integers containing which 
 * @param[out] matchlen  Length of matchv. That is, # of matches and same as return 
 *                       value (if 0-n)
 * @param[out] cvv      cligen variable vector containing vars/values pair for 
 *                       completion
 * @param[out] reason0   If retval = 0, this may be malloced to indicate reason for
 *                       not matching variables, if given. Need to be free:d
 * @retval     0-n       The number of matches in pt . See param matchlen.
 * @retval     -1        Error
 */
static int 
match_pattern_node(cligen_handle h, 
		   cvec         *cvt,
		   cvec         *cvr,
		   parse_tree    pt,
		   int           level, 
		   int           levels,
		   int           hide,
		   int           expandvar,
		   pt_vec       *ptp, 
		   int          *matchv[], 
		   int          *matchlen,
		   cvec         *cvv,
		   char        **reason0
		   )
{
    int        retval = -1;
    char      *str;
    int        i;
    int        match;
    int        matches = 0;
    int        perfect = 0;
    cg_obj    *co;
    cg_obj    *co_match;
    cg_obj    *co_orig;
    int        rest_match = -1;
    int        p;
    int        preference = 0;
    int        exact;
    char      *reason;
    parse_tree ptn={0,};     /* Expanded */
    cg_var    *cv = NULL;
    int        onlyvars = 0; 

    co_match = NULL;
    /* If there are only variables in the list, then keep track of variable match errors 
     * This is logic to hinder error message to relate to variable mismatch
     * if there is a commands on same level with higher prio to match.
     * If all match fails, it is more interesting to understand the match fails
     * on commands, not variables.
     */
    onlyvars = (reason0==NULL)?0:pt_onlyvars(pt);

    for (i=0; i<pt.pt_len; i++){
	if ((co = pt.pt_vec[i]) == NULL)
	    continue;
	reason = NULL;
	/* Either individual token or rest-of-string */
	str = cvec_i_str(ISREST(co)?cvr:cvt, level+1);
	if ((match = match_object(h, str, co, &exact, onlyvars?&reason:NULL)) < 0)
	    goto error;
	if (match){ /* XXX DIFFERS from match_pattern_terminal */
	    assert(reason==NULL);
	    /* Special case to catch rest variable and space delimited
	       arguments after it */
	    if (ISREST(co))
		rest_match = i;
	    if (match_perfect(str, co)){
		if (!perfect){
		    matches = 0;
		    perfect = 1;
		}
	    }
	    else{
		if (perfect)
		    break;
		p = co_pref(co, exact);
		if (p < preference)
		    continue; /* ignore */
		if (p > preference){
		    preference = p;
		    matches = 0; /* Start again at this level */
		}
	    }
	    co_match = co;
	    matches++;
	}
	/* match == 0, co type is variable and onlyvars, then reason is set once
	 * this may not be the best preference, we just set the first
	 */
	if (reason){
	    if (*reason0 == NULL)
		*reason0 = reason;
	    reason = NULL;
	    onlyvars = 0;
	}
    } /* for */
    if (matches != 0 && reason0 && *reason0){
	    free(*reason0);
	    *reason0 = NULL;
	}

    if (matches != 1) {
#ifdef notneeded
	if (matches == 0){
	    cligen_nomatch_set(h, "Unknown command");
	}
	else
	    cligen_nomatch_set(h, "Ambigious command");
#endif
	retval = 0;
	goto done;
    }
    assert(co_match);

    /* co_orig is original object in case of expansion */
    co_orig = co_match->co_ref?co_match->co_ref: co_match;
    if (pt_expand_treeref(h, co_match, &co_match->co_pt) < 0) /* sub-tree expansion */
	goto error; 

    if (co_match->co_type == CO_VARIABLE){
	if ((cv = add_cov_to_cvec(co_match, str, cvv)) == NULL)
	    goto error;
	/* 
	 * Special case: we have matched a REST variable (anything) and
	 * there is more text have this word, then we can match REST
	 */
	if (rest_match != -1){
	    retval = 1;
	    if (*matchlen < 1){
		*matchlen = 1;
		if ((*matchv = realloc(*matchv, (*matchlen)*sizeof(int))) == NULL){
		    fprintf(stderr, "%s: realloc: %s\n", __FUNCTION__, strerror(errno));
		    return -1;
		}
	    }
	    else
		*matchlen = 1;
	    *ptp = pt.pt_vec;
	    (*matchv)[0] = rest_match;
	    goto done;
	}

    }
    else
	if (co_match->co_type == CO_COMMAND && co_orig->co_type == CO_VARIABLE)
	    if ((cv = add_cov_to_cvec(co_orig, co_match->co_command, cvv)) == NULL)
		goto error;
    if (pt_expand_2(h, &co_match->co_pt, cvv, hide, expandvar, &ptn) < 0) /* expand/choice variables */
	goto error;

    if (level+1 == levels)
	retval = match_pattern_terminal(h, cvt, cvr, ptn, 
					level+1, levels, 
					ptp, matchv, matchlen, reason0);
    else
	retval = match_pattern_node(h, cvt, cvr, ptn,
				    level+1, levels,
				    hide, expandvar,
				    ptp, matchv, matchlen, cvv, reason0);
    if (pt_expand_add(co_orig, ptn) < 0) /* add expanded ptn to orig parsetree */
	goto error;
    /* From here ptn is not used (but ptp may be inside ptn) */
    if (co_match->co_type == CO_COMMAND && co_orig->co_type == CO_VARIABLE)
	if (co_value_set(co_orig, co_match->co_command) < 0)
	    goto error;

    /* Cleanup made on top-level */
  done:
    if (cv){ /* cv may be stale */
	cv = cvec_i(cvv, cvec_len(cvv)-1);
	cv_reset(cv);
	cvec_del(cvv, cv);
    }
    /* Only the last level may have multiple matches */
    return retval;
  error:
    retval = -1;
    goto done;
}

/*! CLiIgen object matching function
 * @param[in]  h         CLIgen handle
 * @param[in]  string    Input string to match
 * @param[in]  cvt      Tokenized string: vector of tokens
 * @param[in]  cvr      Rest variant,  eg remaining string in each step
 * @param[in]  pt        Vector of commands (array of cligen object pointers (cg_obj)
 * @param[in]  hide      Respect hide setting of commands (dont show)
 * @param[in]  expandvar Set if VARS should be expanded, eg ? <tab>
 * @param[out] ptp       Returns the vector at the place of matching
 * @param[out] matchv    A vector of integers containing which matches
 * @param[out] matchlen  Length of matchv. That is, # of matches and same as 
 *                       return value (if 0-n)
 * @param[out] cvv      cligen variable vector containing vars/values pair for completion
 * @param[out] reason0   If retval = 0, this may be malloced to indicate reason for 
 *                       not matching variables, if given. Neeed to be free:d
 *
 * @retval -1   error.
 * @retval nr   The number of matches (0-n) in pt or -1 on error. See matchlen below.
 *
 * All options are ordered by PREFERENCE, where 
 *       command > ipv4,mac > string > rest
 */
int 
match_pattern(cligen_handle h,
	      cvec         *cvt,
	      cvec         *cvr,
	      parse_tree    pt, 
	      int           hide,
	      int           expandvar,
	      pt_vec       *ptp, 
	      int          *matchv[],
	      int          *matchlen, 
	      cvec         *cvv,
	      char        **reason0)
{
    int retval = -1;
    int levels;
    int n; /* nr matches */

    assert(ptp && cvt && cvr); /* XXX */

    /* Get total number of command levels */
    if ((levels = cligen_cvv_levels(cvt)) < 0)
	goto done;
    if (levels == 0){
	if ((n = match_pattern_terminal(h, cvt, cvr,
					pt,
					0, levels,
					ptp, matchv, matchlen, 
					reason0)) < 0)
	    goto done;
    }
    else if ((n = match_pattern_node(h, cvt, cvr,
				    pt,
				    0, levels,
				    hide, expandvar,
				    ptp, matchv, matchlen, 
				    cvv,
				    reason0)) < 0)
	    goto done;
    retval = n; /* nr matches */
 done:
    return retval;
}

static int
match_multiple(cligen_handle h,
	       pt_vec        pt,
	       int           matchv[],
	       int          *matchlen)
{
    int     retval = -1;
    int     i;
    cg_obj *co;
    int     preference;
    int     p;
    int     j;

    /* Use preference as tie breaker (can replace above??) */
    j = 0;
    for (i=0; i<*matchlen; i++){
	co = pt[matchv[i]];
	p = co_pref(co, 1);
	if (p < preference)
	    continue; /* ignore */
	if (p > preference){
	    preference = p;
	    j = 0; /* Start again at this level */
	}
	matchv[j++] = matchv[i];
    }
    *matchlen = j;
    /* If set, if multiple cligen variables match use the first one */
    if (cligen_preference_mode(h) && *matchlen>1)
	*matchlen = 1;
    retval = 0;
    //done:
    return retval;
}

/*! CLIgen object matching function for perfect match
 * @param[in]  h         CLIgen handle
 * @param[in]  string    Input string to match
 * @param[in]  cvt       Tokenized string: vector of tokens
 * @param[in]  cvr       Rest variant,  eg remaining string in each step
 * @param[in]  pt        CLIgen parse tree, vector of cligen objects.
 * @param[in]  expandvar Set if VARS should be expanded, eg ? <tab>

 * @param[out] cvv       CLIgen variable vector containing vars/values pair for completion
 * @param[out] match_obj Exact object to return
 * @retval  -1           Error
 * @retval   0           No match
 * @retval   1           Exactly one match and match_obj set
 * @retval   n           More than one match
 * 
 * Only if retval == 0 _AND> exact == 1 then cligen_nomatch() is set, otherwise not.
 * @see match_pattern
 */
int 
match_pattern_exact(cligen_handle h, 
		    cvec         *cvt,
		    cvec         *cvr,
		    parse_tree    pt, 
		    int           expandvar,
		    cvec         *cvv,
		    cg_obj      **match_obj)
{
    int     retval = -1;
    pt_vec  res_pt;
    cg_obj *co;
    int     matchlen = 0;
    int     *matchv = NULL;
    char   *reason = NULL;
    int     i;

        /* clear old errors */
    cligen_nomatch_set(h, NULL); 
    if ((match_pattern(h, cvt, cvr,
		       pt,
		       0, 1,
		       &res_pt, 
		       &matchv, &matchlen,
		       cvv, &reason)) < 0)
	goto done;
    /* Initial match interval */
    switch (matchlen){
    case 0:
	if (reason != NULL){
	    cligen_nomatch_set(h, "%s", reason);
	    free(reason);
	}
	else
	    cligen_nomatch_set(h, "Unknown command");
	break;
    case 1:
	break;
    default: /* Multiple matches, nr > 1 */
	if (match_multiple(h, res_pt, matchv, &matchlen) < 0)
	    goto done;
	break;
    }
    if (matchlen != 1){
	retval = matchlen;
	goto done;
    }
    /* Here we have an obj (res_pt[]) that is unique so far.
       We need to see if there is only one sibling to it. */
    co = res_pt[*matchv];
    /*
     * Special case: if matching object has a NULL child,
     * we match.
     */
    for (i=0; i < co->co_max; i++)
	if (co->co_next[i] == NULL)
	    break;
    if (co->co_max!=0 && i==co->co_max){
	co = NULL;
	cligen_nomatch_set(h, "Incomplete command");
	retval = 0;
	goto done;
    }
    retval = 1;
 done:
    if (match_obj)
	*match_obj = co;
    return retval;
}

/*! Try to complete a string as far as possible using the syntax.
 * 
 * @param[in]     h       cligen handle
 * @param[in]     pt      Vector of commands (array of cligen object pointers)
 * @param[in,out] stringp Input string to match and to complete (append to)
 * @param[in,out] slen    Current string length 
 * @param[out]    cvv    cligen variable vector containing vars/values pair for
 *                        completion
 * @retval    -1   Error 
 * @retval     0   No matches, no completions made
 * @retval     1   Function completed by adding characters at the end of "string"
 */
int 
match_complete(cligen_handle h, 
	       parse_tree    pt, 
	       char        **stringp, 
	       size_t       *slenp, 
	       cvec         *cvv)
{
    int     level;
    int     slen;
    int     equal;
    int     i;
    int     j;
    int     minmatch;
    cg_obj *co;
    cg_obj *co1 = NULL;
    cvec   *cvt = NULL;      /* Tokenized string: vector of tokens */
    cvec   *cvr = NULL;      /* Rest variant,  eg remaining string in each step */
    char   *string;
    char   *s;
    char   *ss;
    pt_vec  pt1;
    int     nr;
    int     matchlen = 0;
    int    *matchv = NULL;
    int     mv;
    int     append = 0; /* Has appended characters */
    int     retval = -1;

    /* ignore any leading whitespace */
    string = *stringp;
    /* Tokenize the string and transform it into two CLIgen vectors: tokens and rests */
    if (cligen_str2cvv(string, &cvt, &cvr) < 0)
	goto done;
    s = string;
    while ((strlen(s) > 0) && isblank(*s))
	s++;
 again:
    matchlen = 0;
    if ((nr = match_pattern(h, cvt, cvr,
			    pt,
			    1, 1, /* Must be one for interactive TAB to work*/
			    &pt1, &matchv, &matchlen, cvv, NULL)) < 0)
	goto done;
    if (nr==0){
	retval = 0;
	goto done; /*  No matches */
    }
    if ((level = cligen_cvv_levels(cvt)) < 0)
	goto done;
    ss = cvec_i_str(cvt, level+1);
    slen = ss?strlen(ss):0;

    minmatch = slen;
    equal = 1;
    for (i=0; i<matchlen; i++){
	mv = matchv[i];
	assert(mv != -1);
	co = pt1[mv];
	if (co == NULL){
	    retval = 0;
	    goto done;
	}
	if ((cligen_tabmode(h) & CLIGEN_TABMODE_VARS) == 0)
	    if (co->co_type != CO_COMMAND)
		continue;
	if (co1 == NULL){
	    minmatch = strlen(co->co_command);
	    co1 = co;
	}
	else{
	    if (strcmp(co1->co_command, co->co_command)==0)
		; /* equal */
	    else{
		equal = 0;
		for (j=0; j<MIN(strlen(co1->co_command), strlen(co->co_command)); j++)
		    if (co1->co_command[j] != co->co_command[j])
			break;
		minmatch = MIN(minmatch, j);
	    }
	}
    }
    if (co1 == NULL){
        retval = 0;
	goto done;
    }
    while (strlen(*stringp) + minmatch - slen >= *slenp){
	*slenp *= 2;
	if ((*stringp = realloc(*stringp, *slenp)) == NULL){
	    fprintf(stderr, "%s realloc: %s\n", __FUNCTION__, strerror(errno));
	    goto done;
	}
	string = *stringp;
    }
    strncat(string, &co1->co_command[slen], minmatch-slen);
    append = append || minmatch-slen;
    if (equal){ /* add space */
	string[strlen(string)+1] = '\0';
	string[strlen(string)] = cligen_delimiter(h);
	level++;
	slen = 0;
	co1 = NULL;
	if (cligen_tabmode(h)&CLIGEN_TABMODE_STEPS){
	    if (matchv)
		free(matchv);
	    matchv = NULL;
	    goto again;
	}
    }
    retval = append?1:0;
  done:
    if (cvt)
	cvec_free(cvt);
    if (cvr)
	cvec_free(cvr);
    if (matchv)
	free(matchv);
    return retval;
}

