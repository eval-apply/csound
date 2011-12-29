/*
    new_orc_parser.c:

    Copyright (C) 2006
    Steven Yi
    Modifications 2009 by Christopher Wilson for multicore

    This file is part of Csound.

    The Csound Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    Csound is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with Csound; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
    02111-1307 USA
*/

#include "csoundCore.h"
#include "csound_orcparse.h"
#include "csound_orc.h"
#include "parse_param.h"
#include "corfile.h"

//#include "yyguts.h"

#define ST(x)   (((RDORCH_GLOBALS*) csound->rdorchGlobals)->x)

extern void csound_orcrestart(FILE*, void *);

extern int csound_orcdebug;

extern void print_csound_predata(void *);
extern void csound_prelex_init(void *);
extern void csound_preset_extra(void *, void *);
extern void csound_prelex(CSOUND*, void*);
extern void csound_prelex_destroy(void *);
extern void csound_orc_scan_buffer (const char *, size_t, void*);
extern int csound_orcparse(PARSE_PARM *, void *, CSOUND*, TREE*);
extern void csound_orclex_init(void *);
extern void csound_orcset_extra(void *, void *);
//extern void csound_orc_scan_string(char *, void *);
extern void csound_orcset_lineno(int, void*);
extern void csound_orclex_destroy(void *);
extern void init_symbtab(CSOUND*);
extern void print_tree(CSOUND *, char *, TREE *);
extern TREE* verify_tree(CSOUND *, TREE *);
extern TREE *csound_orc_expand_expressions(CSOUND *, TREE *);
extern TREE* csound_orc_optimize(CSOUND *, TREE *);
extern void csound_orc_compile(CSOUND *, TREE *);
#ifdef PARCS
extern TREE *csp_locks_insert(CSOUND *csound, TREE *root);
void csp_locks_cache_build(CSOUND *);
void csp_weights_calculate(CSOUND *, TREE *);
#endif


int new_orc_parser(CSOUND *csound)
{
    int retVal;
    TREE* astTree = (TREE *)mcalloc(csound, sizeof(TREE));
    OPARMS *O = csound->oparms;
    PRE_PARM    qq;
    PARSE_PARM  pp;
    /* Preprocess */
    corfile_puts("\n#exit\n", csound->orchstr);
    memset(&qq, '\0', sizeof(PRE_PARM));
    csound_prelex_init(&qq.yyscanner);
    csound_preset_extra(&qq, qq.yyscanner);
    qq.line = 1;
    csound->expanded_orc = corfile_create_w();
    fprintf(stderr, "Calling preprocess on >>%s<<\n", corfile_body(csound->orchstr));
    //print_csound_predata("start", &qq.yyscanner);
    cs_init_math_constants_macros(csound, qq.yyscanner);
    cs_init_omacros(csound, qq.yyscanner, csound->omacros);
    csound_prelex(csound, &qq.yyscanner);
    if (UNLIKELY(qq.ifdefStack != NULL)) {
      csound->Message(csound, Str("Unmatched #ifdef\n"));
      csound->LongJmp(csound, 1);
    }
    csound_prelex_destroy(pp.yyscanner);
    fprintf(stderr, "yielding >>%s<<\n", corfile_body(csound->expanded_orc));
    /* Parse */
    memset(&pp, '\0', sizeof(PARSE_PARM));
    init_symbtab(csound);

    //    pp.buffer = (char*)csound->Calloc(csound, lMaxBuffer);

    csound_orcdebug = O->odebug;
    csound_orclex_init(&pp.yyscanner);
    //    yyg = (struct yyguts_t*)pp.yyscanner;

    csound_orcset_extra(&pp, pp.yyscanner);
    csound_orc_scan_buffer(corfile_body(csound->expanded_orc),
                           corfile_tell(csound->expanded_orc), pp.yyscanner);
    free(csound->expanded_orc);
    /*     These relate to file input only       */
    /*     csound_orcset_in(ttt, pp.yyscanner); */
    /*     csound_orcrestart(ttt, pp.yyscanner); */
    //csound_orcset_lineno(csound->orcLineOffset, pp.yyscanner);
    retVal = csound_orcparse(&pp, pp.yyscanner, csound, astTree);
    if (csound->synterrcnt) retVal = 3;
    if (LIKELY(retVal == 0)) {
      csound->Message(csound, "Parsing successful!\n");
    }
    else {
      if (retVal == 1){
        csound->Message(csound, "Parsing failed due to invalid input!\n");
      }
      else if (retVal == 2){
        csound->Message(csound, "Parsing failed due to memory exhaustion!\n");
      }
      else if (retVal == 3){
        csound->Message(csound, "Parsing failed due to %d syntax error%s!\n",
                        csound->synterrcnt, csound->synterrcnt==1?"":"s");
      }
      goto ending;
    }
    if (UNLIKELY(PARSER_DEBUG)) {
      print_tree(csound, "AST - INITIAL\n", astTree);
    }

    astTree = verify_tree(csound, astTree);
#ifdef PARCS
    if (LIKELY(O->numThreads > 1)) {
      /* insert the locks around global variables before expr expansion */
      astTree = csp_locks_insert(csound, astTree);
      csp_locks_cache_build(csound);
    }
#endif /* PARCS */

    astTree = csound_orc_expand_expressions(csound, astTree);

    if (UNLIKELY(PARSER_DEBUG)) {
      print_tree(csound, "AST - AFTER EXPANSION\n", astTree);
    }     
#ifdef PARCS
    if (LIKELY(O->numThreads > 1)) {
      /* calculate the weights for the instruments */
      csp_weights_calculate(csound, astTree);
    }
#endif /* PARCS */
 
    astTree = csound_orc_optimize(csound, astTree);
    csound_orc_compile(csound, astTree);

 ending:
    corfile_rm(&csound->orchstr);
    csound_orclex_destroy(pp.yyscanner);
    return retVal;
}

