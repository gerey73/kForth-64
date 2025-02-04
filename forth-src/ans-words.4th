\ ans-words.4th
\
\ Some ANS Forth words which are not a part of the intrinsic
\ dictionary of kForth are implemented here in source code.
\ Use with kForth version 1.4.x or higher.
\
\ Some other words, which are not part of the ANS specification,
\ but which are so commonly used that they are effectively
\ standard Forth words, are also defined here.
\
\ Also, see the following files for the source code definitions 
\ of other ANS Forth words which are not a part of kForth's 
\ dictionary:   
\ 
\     strings.4th 
\     files.4th 
\     ansi.4th 
\     dump.4th
\
\ Copyright (c) 2002--2011 Krishna Myneni, Creative Consulting
\   for Research and Education
\
\ Provided under the GNU Lesser General Public License (LGPL)
\
\ Revisions:
\   2002-09-06  Created    km
\   2002-10-27  added F~   km
\   2003-02-15  added D2*, D2/, DMIN, DMAX, 2CONSTANT, 2VARIABLE, 
\                 2LITERAL  km
\   2003-03-02  fixed F~ for case of exact equality test  km
\   2003-03-09  added >NUMBER, DEFER, and IS  km
\   2003-09-28  added [IF], [ELSE], [THEN]  km
\   2004-09-10  added CATCH and THROW  km
\   2005-09-19  added [DEFINED] and [UNDEFINED]  km
\   2005-09-28  commented out defn of D>S km
\   2006-04-06  replaced M*/ by UDM* for ppc version dnw
\   2006-04-09  removed DMIN, DMAX, now intrinsic dnw
\   2006-05-30  commented out MOVE, now intrinsic km
\   2007-07-15  removed obsolete defs, which were commented out  km
\   2008-03-16  removed 2CONSTANT and 2VARIABLE, now intrinsic  km
\   2008-03-28  removed 2LITERAL, now intrinsic  km
\   2009-09-20  removed >NUMBER, now intrinsic  km
\   2009-09-26  removed WITHIN, now intrinsic  km
\   2009-10-01  modified [ELSE] to be case insensitive  km
\   2009-11-26  removed D2* and D2/, now intrinsic  km
\   2010-12-23  added $ucase and revised [ELSE] to use $ucase  km
\   2011-02-05  km  removed [DEFINED] and [UNDEFINED], now intrinsic 
BASE @
DECIMAL
\ ============== From the CORE wordset

: SPACE BL EMIT ;
: CHARS ;

\ ============ From the CORE EXT wordset

CREATE PAD 512 ALLOT

: TO ' >BODY STATE @ IF POSTPONE LITERAL POSTPONE ! ELSE ! THEN ; IMMEDIATE
: VALUE CREATE 1 CELLS ?ALLOT ! DOES> @ ;


\ ============ From the FLOATING EXT wordset

: F~ ( f1 f2 f3 -- flag )
     FDUP 0e F> 
     IF 2>R F- FABS 2R> F<
     ELSE FDUP F0=
       IF FDROP		  \ are f1 and f2 *exactly* equal 
         ( F=)		  \ F= cannot distinguish between -0e and 0e
	 D=
       ELSE FABS 2>R FOVER FABS FOVER FABS F+ 2>R
         F- FABS 2R> 2R> F* F<
       THEN
     THEN ;
 

\ ============ From the PROGRAMMING TOOLS wordset

\ $ucase is not a standard word; it is provided here as a helper.
: $ucase ( a u -- a u )  \ transform string to upper case
     2DUP  0 ?DO                    
       DUP C@ 
       DUP [CHAR] a [ CHAR z 1+ ] LITERAL WITHIN 
       IF 95 AND THEN OVER C! 1+
     LOOP  DROP ;

( see DPANS94, sec. A.15)

: [ELSE]  ( -- )
    1 BEGIN                                  \ level
      BEGIN
        BL WORD COUNT DUP  WHILE            \ level adr len
            $ucase
	    2DUP  S" [IF]"  COMPARE 0=
            IF                               \ level adr len
              2DROP 1+                       \ level'
            ELSE                             \ level adr len
	      2DUP  S" [ELSE]"  COMPARE 0=
	      IF                             \ level adr len
                2DROP 1- DUP IF 1+ THEN      \ level'
	      ELSE                           \ level adr len
	        S" [THEN]"  COMPARE 0=
	        IF                           \ level
                  1-                         \ level'
                THEN
              THEN
            THEN ?DUP 0=  IF EXIT THEN       \ level'
      REPEAT  2DROP                          \ level
    REFILL 0= UNTIL                          \ level
    DROP
;  IMMEDIATE

: [IF]  ( flag -- )
   0= IF POSTPONE [ELSE] THEN ;  IMMEDIATE

: [THEN]  ( -- )  ;  IMMEDIATE


\ ============= From the EXCEPTION wordset
( see DPANS94, sec. A.9 )

variable handler
: empty-handler ;

' empty-handler  handler !

: CATCH ( xt -- exception# | 0 )
    SP@ >R  ( xt )  \ save data stack pointer
    HANDLER A@ >R   \ and previous handler
    RP@ HANDLER !   \ save return point for THROW
    EXECUTE	    \ execute returns if no THROW
    R> HANDLER !    \ restore previous handler
    R> DROP         \ discard saved state
    0               \ normal completion
;

: THROW ( ??? exception# -- ??? exception# )
    ?DUP IF
      HANDLER A@ RP!   \ restore previous return stack
      R> HANDLER !     \ restore prev handler
      R> SWAP >R
      SP! DROP R>      \ restore stack
        \  Return to the caller of CATCH because return
        \  stack is restored to the state that existed
        \  when CATCH began execution
    THEN
;

\ ============= Forth 200x Standard Words

: DEFER  ( "name" -- )
      CREATE 1 CELLS ?ALLOT ['] ABORT SWAP ! DOES> A@ EXECUTE ;

: IS    ( xt "name" -- )
      '
      STATE @ IF
        postpone LITERAL postpone >BODY postpone !
      ELSE
        >BODY !
      THEN ; IMMEDIATE

BASE !
