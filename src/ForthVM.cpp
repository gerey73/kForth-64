// ForthVM.cpp
//
// The C++ portion of the kForth Virtual Machine to 
// execute Forth byte code.
//
// Copyright (c) 1996--2018 Krishna Myneni,
//   <krishna.myneni@ccreweb.org>
//
// This software is provided under the terms of the GNU
// Affero General Public License, (AGPL), v3.0 or later.

const char* dir_env_var=DIR_ENV_VAR;

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>
using namespace std;

#include "fbc.h"
#include "ForthCompiler.h"
#include "ForthVM.h"
#include "kfmacros.h"

#define STACK_SIZE 32768
#define RETURN_STACK_SIZE 4096

extern bool debug;

// Provided by ForthCompiler.cpp

extern WordTemplate RootWords[];
extern WordTemplate ForthWords[];
extern const char* C_ErrorMessages[];
extern long int linecount;
extern istream* pInStream ;    // global input stream
extern ostream* pOutStream ;   // global output stream
extern vector<byte>* pCurrentOps;
extern vector<int> ifstack;
extern vector<int> beginstack;
extern vector<int> whilestack;
extern vector<int> dostack;
extern vector<int> querydostack;
extern vector<int> leavestack;
extern vector<int> recursestack;
extern vector<int> casestack;
extern vector<int> ofstack;
extern WordListEntry NewWord;
extern size_t NUMBER_OF_INTRINSIC_WORDS;

extern "C" {

  // functions provided by vmc.c

  void  set_start_time(void);
  void  save_term(void);
  void  restore_term(void);
  void  strupr (char*);
  char* ExtractName(char*, char*);
  int   IsFloat(char*, double*);
  int   IsInt(char*, int*);
  int   isBaseDigit(int);
  int   C_bracketsharp(void);
  int   C_sharps(void);
  int   C_sharpbracket(void);
  int   C_word(void);

  // vm functions provided by vm.s/vm-fast.s

  int L_initfpu();
  int L_depth();
  int L_abort();
  int L_ret();
  int L_dabs();
  int vm (byte*);     // the machine code virtual machine

  // global pointers exported to other modules

  long int* GlobalSp;      // the global stack pointer
  byte* GlobalIp;     // the global instruction pointer
  long int* GlobalRp;      // the global return stack pointer
  long int* BottomOfStack;
  long int* BottomOfReturnStack;

#ifndef __FAST__
  byte* GlobalTp;     // the global type stack pointer
  byte* GlobalRtp;    // the global return type stack pointer
  byte* BottomOfTypeStack;
  byte* BottomOfReturnTypeStack;
#endif

  long int* vmEntryRp;
  long int Base;
  long int State;
  long int Precision;
  char* pTIB;
  long int NumberCount;
  char WordBuf[256];
  char TIB[256];
  char NumberBuf[256];
  char ParseBuf[1024];

}
extern "C" long int JumpTable[];
extern "C" void dump_return_stack(void); 

// The Dictionary

vector<Vocabulary*> Dictionary;          // a collection of vocabularies
SearchList SearchOrder;                  // the search order for wordlists/vocabularies
Vocabulary Voc_Root( "Root" );           // the minimum search order vocabulary
Vocabulary Voc_Forth( "Forth" );         // the Forth vocabulary
Vocabulary Voc_Assembler( "Assembler" ); // the assembler vocabulary
WordList *pCompilationWL = &Voc_Forth;   // the current compilation wordlist

// Tables
vector<char*> StringTable;               // table for persistent strings

// stacks; these are global to this module

long int ForthStack[STACK_SIZE];                  // the stack
long int ForthReturnStack[RETURN_STACK_SIZE];     // the return stack
#ifndef __FAST__
byte ForthTypeStack[STACK_SIZE];             // the value type stack
byte ForthReturnTypeStack[RETURN_STACK_SIZE];// the return value type stack
#endif


bool FileOutput = FALSE;
vector<byte>* pPreviousOps;    // copy of ptr to old opcode vector for [ and ]
vector<byte> tempOps;          // temporary opcode vector for [ and ]

const char* V_ErrorMessages[] =
{
	"",
	"Not data type ADDR",
	"Not data type IVAL",
	"Invalid data type",	
	"Divide by zero",
	"Return stack corrupt",
	"Invalid opcode", 
        "Stack underflow",
	"",
	"Allot failed --- cannot reassign pfa",
	"Cannot create word",
	"End of string not found",
	"No matching DO",
	"No matching BEGIN",
	"ELSE without matching IF",
	"THEN without matching IF",
	"ENDOF without matching OF",
	"ENDCASE without matching CASE",
	"Cannot open file",
	"Address outside of stack space",
	"Division overflow",
        "Unsigned double number overflow"
};
//---------------------------------------------------------------

void WordList::RemoveLastWord ()
{
// Remove the last entry in the current wordlist

	WordIndex i = end() - 1;
	delete [] (byte*) i->Pfa;	// free memory
	if (i->Pfa != i->Cfa) delete [] (byte*) i->Cfa;
	pop_back(); 
}

WordIndex WordList::IndexOf ( char* name )
{
   // Return the index (iterator) of the most recently defined
   //   word of the specified name
   WordIndex i;
   if (size()) {
	for (i = end()-1; i >= begin(); --i)
		if (*((word*)name) == *((word*) i->WordName)) // pre-compare
			if (strcmp(name, i->WordName) == 0) return( i );
   }
   return( end() );
}

bool WordList::RetrieveFromName (char* name, WordListEntry* pWord)
{
     WordIndex i = IndexOf( name );
     bool b = i < end();
     if (b) *pWord = *i;
     return b;
}

bool WordList::RetrieveFromCfa (void* cfa, WordListEntry* pWord)
{
     WordIndex i;
     
     if (size()) {
	for (i = end()-1; i >= begin(); --i)
	  	if (cfa == i->Cfa)  {
			*pWord = *i;
			return( true );
		}
     }
    return( false );
}

//---------------------------------------------------------------

Vocabulary::Vocabulary( const char* name )
{
    int n = strlen(name);
    char *cp = new char [n+1];
    strcpy( cp, name );
    StringTable.push_back(cp);
    Name = cp;
}

int Vocabulary::Initialize( WordTemplate wt[], int n )
{
    int i, wcode;
    WordListEntry w;

    for (i = 0; i < n; i++)
    {
        strcpy(w.WordName, wt[i].WordName);
        wcode = wt[i].WordCode;
	w.WordCode = wcode;
	w.Precedence = wt[i].Precedence;
        w.Pfa = new byte[WSIZE+2];
	w.Cfa = w.Pfa;
	byte* bp = (byte*) w.Pfa;
	if (wcode >> 8)
	{
	    bp[0] = OP_CALLADDR;
	    *((long int*) (bp+1)) = (long int) JumpTable[wcode];
	    bp[WSIZE+1] = OP_RET;
	}
	else
	{
	    bp[0] = wcode;
	    bp[1] = OP_RET;
	}
	
        push_back(w);
    }
    return 0;
}

//---------------------------------------------------------------

bool SearchList::LocateWord (char* name, WordListEntry* pWord)
{
// Iterate through the search list, to look for an entry
//   with the specified name. If found, copy the WordListEntry at
//   the specified pointer, and return true. Return false if not found.

    	vector<Vocabulary*>::iterator j;
	bool b = false;
	for (j = begin(); j < end(); ++j) {
          b = (*j)->RetrieveFromName( name, pWord );
	  if (b) return b;
	}
	return false;
}

bool SearchList::LocateCfa (void* cfa, WordListEntry* pWord)
{
// Iterate through the search list, to look for an entry
//   with the specified Cfa. If found, copy the WordListEntry at 
//   the specified pointer, and return true. Return false if not found.

    	vector<Vocabulary*>::iterator j;
	bool b = false;

	for (j = begin(); j < end(); ++j) {
          b = (*j)->RetrieveFromCfa( cfa, pWord );
          if (b) return b;
	}
	return false;
}
//---------------------------------------------------------------

int OpenForth ()
{
// Initialize the Forth system, and return the total number of words
//   in the dictionary.

    // The Dictionary initially contains the Root, Forth, and Assembler 
    // wordlists.
    Voc_Root.Initialize(RootWords, 5);
    Dictionary.push_back(&Voc_Root);
    Voc_Forth.Initialize(ForthWords, NUMBER_OF_INTRINSIC_WORDS);
    Dictionary.push_back(&Voc_Forth);
    Dictionary.push_back(&Voc_Assembler); 

    // The SearchOrder initially provides the Root and Forth wordlists
    // and the initial compilation wordlist is Forth
    SearchOrder.push_back(&Voc_Forth);
    SearchOrder.push_back(&Voc_Root);
    pCompilationWL = &Voc_Forth;

    // Initialize the global stack pointers
    BottomOfStack = ForthStack + STACK_SIZE - 1;
    BottomOfReturnStack = ForthReturnStack + RETURN_STACK_SIZE - 1;
    GlobalSp = BottomOfStack;
    GlobalRp = BottomOfReturnStack;
#ifndef __FAST__
    BottomOfTypeStack = ForthTypeStack + STACK_SIZE - 1;
    BottomOfReturnTypeStack = ForthReturnTypeStack + RETURN_STACK_SIZE - 1;
    GlobalTp = BottomOfTypeStack;
    GlobalRtp = BottomOfReturnTypeStack;
#endif

   // Other initialization
    vmEntryRp = BottomOfReturnStack;
    Base = 10;
    State = FALSE;
    Precision = 15;
    set_start_time();
    save_term();
    L_initfpu();

    return( Voc_Forth.size() + Voc_Root.size() );
}
//---------------------------------------------------------------

void CloseForth ()
{
    // Clean up the compiled words
    Vocabulary* pVoc;

    while (Dictionary.size())
    {
        pVoc = *(Dictionary.end() - 1);
    	while (pVoc->size())
    	{
        	pVoc->RemoveLastWord();
    	}
	Dictionary.pop_back();
   }

    // Clean up the string table

    vector<char*>::iterator j = StringTable.begin();

    while (j < StringTable.end())
    {
        if (*j) delete [] *j;
        ++j;
    }
    StringTable.erase(StringTable.begin(), StringTable.end());

    restore_term();
}

//---------------------------------------------------------------

bool InStringTable(char *s)
{
    // Search for a string pointer in the StringTable
    vector<char*>::iterator j = StringTable.begin();

    // *pOutStream << "Checking StringTable for pointer " << (int) s << " ... ";
    while (j < StringTable.end())
    {
        if (*j == s) {
	  // *pOutStream << " found\n";
	  return(true);
	}
        ++j;
    }
    // *pOutStream << " NOT found\n";
    return(false);
}
//---------------------------------------------------------------

void ClearControlStacks ()
{
  // Clear the flow control stacks

  if (debug) cout << "Clearing all flow control stacks" << endl; 
  ifstack.erase(ifstack.begin(), ifstack.end());
  beginstack.erase(beginstack.begin(),beginstack.end());
  whilestack.erase(whilestack.begin(),whilestack.end());
  dostack.erase(dostack.begin(), dostack.end());
  querydostack.erase(querydostack.begin(), querydostack.end());
  leavestack.erase(leavestack.begin(), leavestack.end());
  ofstack.erase(ofstack.begin(), ofstack.end());
  casestack.erase(casestack.begin(), casestack.end());
}
//---------------------------------------------------------------

void OpsCopyInt (long int offset, long int i)
{
  // Copy integer into the current opcode vector at the specified offset 

  vector<byte>::iterator ib = pCurrentOps->begin() + offset;
  byte* ip = (byte*) &i;
  for (unsigned int j = 0; j < WSIZE; j++) *(ib+j) = *(ip + j);
//  *ib++ = *ip; *ib++ = *(ip + 1); *ib++ = *(ip + 2); *ib = *(ip + 3);

}
//---------------------------------------------------------------

void OpsPushInt (long int i)
{
  // push an integer into the current opcode vector

  byte* ip = (byte*) &i;
  for (unsigned int j = 0; j < WSIZE; j++) pCurrentOps->push_back(*(ip + j));
}
//---------------------------------------------------------------

void OpsPushDouble (double f)
{
  // push a floating point double into the current opcode vector

  byte* fp = (byte*) &f;
  for (unsigned int j = 0; j < sizeof(double); j++) pCurrentOps->push_back(*(fp + j));
}
//---------------------------------------------------------------

int OpsCompileByte ()
{
  // push a byte value from the stack into the current opcode vector

  int endian = 1;
  DROP
  byte* ip = (byte*) GlobalSp;
  ip += (*((byte*) &endian)) ? 0 : sizeof(long int)-1; // handle big or little endian
  pCurrentOps->push_back(*ip);
  return 0;
}
//---------------------------------------------------------------

int OpsCompileInt ()
{
  // push an int value from the stack into the current opcode vector

  DROP
  OpsPushInt(TOS);
  return 0;
}
//---------------------------------------------------------------

int OpsCompileDouble ()
{
  // push a double value from the stack into the current opcode vector

  DROP
  OpsPushInt(TOS);
  DROP
  OpsPushInt(TOS);
  return 0;
}
//----------------------------------------------------------------

void PrintVM_Error (long int ec)
{
    int ei = ec & 0xFF;
    int imax = (ec >> 8) ? MAX_C_ERR_MESSAGES : MAX_V_ERR_MESSAGES;
    const char *pMsg;
    char elabel[12];
    
    if ((ei >= 0) && (ei < imax))
    {
	pMsg = (ec >> 8) ? C_ErrorMessages[ei] : V_ErrorMessages[ei];
	if (ec >> 8)  strcpy(elabel, "Compiler"); 
	else strcpy(elabel, "VM");
	*pOutStream << elabel << " Error(" << ei << "): " <<
	    pMsg << endl;
   }
}
//---------------------------------------------------------------

int ForthVM (vector<byte>* pFBC, long int** pStackPtr, byte** pTypePtr)
{
// The FORTH Virtual Machine
//
// Arguments:
//
//      pFBC        pointer to vector of Forth byte codes
//      pStackPtr   receives pointer to the top item on the stack at exit
//      pTypePtr    receives pointer to the top item on the type stack at exit
//
// Return value: error code (see ForthVM.h)
//

  if (pFBC->size() == 0) return 0;  // null opcode vector

  // Initialize the instruction ptr and error code

  // byte *ip = (byte*) pFBC->begin();
  byte *ip = (byte *) &(*pFBC)[0];
  long int ecode = 0;

if (debug)  {
	cout << ">ForthVM Sp: " << GlobalSp << " Rp: " << GlobalRp << endl;
	cout << "  Ip: " << (long int) ip << " *Ip: " << (long int) *ip << endl;
	}
  // Execute the virtual machine; return when error occurs or
  //   the return stack is exhausted.

  ecode = vm (ip);

  if (ecode & 0xff)
    {
      if (debug) cout << "vm Error: " << ecode << endl; // "  Offending OpCode: " << ((int) *(GlobalIp-1)) << endl;
      ClearControlStacks();
      GlobalRp = BottomOfReturnStack;        // reset the return stack ptrs
#ifndef __FAST__
      GlobalRtp = BottomOfReturnTypeStack;
#endif  
    }
  else if (GlobalSp > BottomOfStack)
  {
      ecode = E_V_STK_UNDERFLOW;
  }
  else if (GlobalRp > BottomOfReturnStack)
  {
      ecode = E_V_RET_STK_CORRUPT;
  }
  else
      ;

  // On stack underflow, update the global stack pointers.

  if ((ecode == E_V_STK_UNDERFLOW) || (ecode == E_V_RET_STK_CORRUPT))
  {
      L_abort();
  }

  // Set up return information

  *pStackPtr = GlobalSp + 1;
#ifndef __FAST__
  *pTypePtr = GlobalTp + 1;
#endif
if (debug)  cout << "<ForthVM Sp: " << GlobalSp << " Rp: " << GlobalRp << 
	      "  vmEntryRp: " << vmEntryRp << endl;
  return ecode;
}
//---------------------------------------------------------------

// Use C linkage for all of the VM functions

extern "C" {

int CPP_wordlist()
{
     // Create a new wordlist
     // stack: ( -- wid)
     Vocabulary* pVoc = new Vocabulary(""); // create an unnamed vocabulary
     *GlobalSp-- = (long int) pVoc;
     STD_ADDR
     Dictionary.push_back(pVoc); 
     return 0;
}

int CPP_forthwordlist()
{
     // Return the Forth wordlist
     // stack: ( -- wid)
     *GlobalSp-- = (long int) &Voc_Forth ;
     STD_ADDR
     return 0;
}

int CPP_getcurrent()
{
     // Return the compilation (current) wordlist
     // stack: ( -- wid)
     *GlobalSp-- = (long int) pCompilationWL;
     STD_ADDR
     return 0;
}

int CPP_setcurrent()
{
     // Set the compilation (current) wordlist
     // stack: ( wid -- )
     DROP
     CHK_ADDR
     pCompilationWL = (WordList*) TOS;
     return 0;
}

int CPP_getorder()
{
      // Return the current search order
      // stack: ( -- widn ... wid1 n)
     vector<Vocabulary*>::iterator i;

     if (SearchOrder.size()) {
       for ( i = SearchOrder.end()-1; i >= SearchOrder.begin(); i--) {
	  *GlobalSp-- = (long int) (*i);
          STD_ADDR
       }
     }
     *GlobalSp-- = SearchOrder.size();
     STD_IVAL
     return 0;
}

int CPP_setorder()
{
      // Set the search order
      // stack: ( widn ... wid1 n -- )
      DROP
      long int nWL = TOS;
      if (nWL == -1) 
        CPP_only();
      else
      {
        SearchOrder.clear();
        for (int i = 0; i < nWL; i++) {
          DROP
	  CHK_ADDR
	  SearchOrder.push_back((Vocabulary*) TOS);
        }
      }
      return 0;
}

int CPP_searchwordlist()
{
      // Search for the word in the specified wordlist
      // stack: ( c-addr u wid -- 0 | xt 1 | xt -1)
      DROP
      CHK_ADDR
      WordList* pWL = (WordList*) TOS;
      DROP
      long int len = TOS;
      DROP
      CHK_ADDR
      char* cp = (char*) TOS;
      if (len > 0) {
	char* name = new char [len+1];
        strncpy(name, cp, len);
        name[len] =  0;
	strupr(name);
	WordListEntry w;
        bool b = pWL->RetrieveFromName(name, &w);
	delete [] name;
        if (b) {
          PUSH_ADDR((long int) w.Cfa)
          PUSH_IVAL( (w.Precedence & PRECEDENCE_IMMEDIATE) ? 1 : -1 )
          return 0;
	}
      }
      PUSH_IVAL(0)
      return 0;
}

int CPP_definitions()
{
     // Make the compilation wordlist the same as the first search order wordlist
     // stack: ( -- )
     if (SearchOrder.size()) pCompilationWL = SearchOrder.front();
     return 0;
}

int CPP_vocabulary()
{
     // Make a new vocabulary
     // stack: ( "name" -- )
     CPP_create();
     WordIndex iWord = pCompilationWL->end() - 1;
     Vocabulary *pVoc = new Vocabulary(iWord->WordName);
     Dictionary.push_back(pVoc);

     iWord->Precedence = PRECEDENCE_NON_DEFERRED;
     iWord->Pfa = NULL;
     byte* bp = new byte[3*WSIZE+6];
     iWord->Cfa = bp;
     iWord->WordCode = OP_DEFINITION;
    
     // Execution behavior of new word:
     // get-order nip pVoc swap set-order
     bp[0] = OP_CALLADDR;
     *((long int*)(bp+1)) = (long int) JumpTable[OP_GETORDER]; 
     bp[WSIZE+1] = OP_NIP;
     bp[WSIZE+2] = OP_ADDR;
     *((long int*)(bp+WSIZE+3)) = (long int) pVoc;
     bp[2*WSIZE+3] = OP_SWAP;
     bp[2*WSIZE+4] = OP_CALLADDR;
     *((long int*) (bp+2*WSIZE+5)) = (long int) JumpTable[OP_SETORDER];
     bp[3*WSIZE+5] = OP_RET;

     return 0;
}

int CPP_only()
{
     // Make the Forth wordlist the current wordlist and the only
     //   wordlist in the search order.
     // stack: ( -- )
     SearchOrder.clear();
     SearchOrder.push_back(&Voc_Root);
     return 0;
}

int CPP_also()
{
     // Duplicate the first wordlist in the search order
     // stack: ( -- )
     SearchOrder.insert(SearchOrder.begin(), SearchOrder.front());
     return 0;
}

int CPP_order()
{
     // Display the wordlist search order with the current compilation wordlist
     //   in brackets
     Vocabulary* pVoc;
     vector<Vocabulary*>::iterator i;
     const char* cp;
     const char* pUnnamed = "Unnamed";

     for ( i = SearchOrder.begin(); i < SearchOrder.end(); i++) {
        pVoc = *i;
        cp = pVoc->Name;
        if (! *cp) cp = pUnnamed; 
        if (pVoc == pCompilationWL)
	  *pOutStream << "[" << cp << "]  ";
	else         
	  *pOutStream << cp << "  ";
     }
     return 0;
}

int CPP_previous()
{
     // Remove the first wordlist in the search order
     SearchOrder.erase(SearchOrder.begin());
     return 0;
}


int CPP_forth()
{
     // Make the first wordlist in the search order be the Forth wordlist.
     // Ensure that the Root wordlist remains in the search order
     // stack: ( -- )
     if (SearchOrder.size() == 1) CPP_also();
     SearchOrder[0] = &Voc_Forth;
     return 0;
}

int CPP_assembler()
{
    // stack: ( -- | make the Assembler wordlist the current wordlist)
    SearchOrder[0] = &Voc_Assembler;
    return 0;
}
//----------------------------------------------------------------

int CPP_colon()
{
    // stack: ( -- | the colon compiler )

    char WordToken[256];
    State = TRUE;
    pTIB = ExtractName (pTIB, WordToken);
    strupr(WordToken);
    strcpy (NewWord.WordName, WordToken);
    NewWord.WordCode = OP_DEFINITION;
    NewWord.Precedence = PRECEDENCE_NONE;
    NewWord.Pfa = NULL;
    NewWord.Cfa = NULL;
    recursestack.erase(recursestack.begin(), recursestack.end());

    return 0;
}

int CPP_semicolon()
{
  // stack: ( -- | terminate compilation of word )

  int ecode = 0;

  pCurrentOps->push_back(OP_RET);

  if (State)
    {
      // Check for incomplete control structures
		    
      if (ifstack.size())                          ecode = E_C_INCOMPLETEIF;

      if (beginstack.size() || whilestack.size())  ecode = E_C_INCOMPLETEBEGIN;
      
      if (dostack.size() || leavestack.size())     ecode = E_C_INCOMPLETELOOP;

      if (casestack.size() || ofstack.size())      ecode = E_C_INCOMPLETECASE;
      
      if (ecode) return ecode;

      // Add a new entry into the dictionary

      if (debug) OutputForthByteCode (pCurrentOps);
 		  
      NewWord.Pfa = new byte[pCurrentOps->size()];
      NewWord.Cfa = NewWord.Pfa;

      // Resolve any self references (recursion)

      byte *bp, *dest;
      unsigned long int i;
      vector<byte>::iterator ib;
      WordListEntry d;


      bp = (byte*) &NewWord.Pfa;
      while (recursestack.size())
	{
	  i = recursestack[recursestack.size() - 1];
	  ib = pCurrentOps->begin() + i;
	  for (i = 0; i < sizeof(void*); i++) *ib++ = *(bp + i);
	  recursestack.pop_back();
	}

      dest = (byte*) NewWord.Pfa;
      bp = (byte*) &(*pCurrentOps)[0]; // ->begin();
      while ((vector<byte>::iterator) bp < pCurrentOps->end()) *dest++ = *bp++;
      if (IsForthWord(NewWord.WordName, &d)) {
          WordIndex wi = pCompilationWL->IndexOf( NewWord.WordName );
          if (wi < pCompilationWL->end())
	      *pOutStream << NewWord.WordName << " is redefined\n";
      }
      pCompilationWL->push_back(NewWord);
      pCurrentOps->erase(pCurrentOps->begin(), pCurrentOps->end());
      State = FALSE;
    }
  else
    {
      ecode = E_C_ENDOFDEF;
      // goto endcompile;
    }
    
  return ecode;
}
//-----------------------------------------------------------------

int CPP_lparen()
{
  // stack: ( -- | advance pTIB past end of comment )

  while (TRUE)
    {
      while ((pTIB < (TIB + 255)) && (! (*pTIB == ')')) && *pTIB) ++pTIB;
      if (*pTIB == ')')
	{
	  ++pTIB;
	  break;
	}
      else
	{
	  pInStream->getline(TIB, 255);
	  if (pInStream->fail()) return E_V_NO_EOS;
	  ++linecount;
	  pTIB = TIB;
	}
    }

  return 0;
}
//---------------------------------------------------------------

int CPP_dotparen()
{
  // stack: ( -- | display comment and advance pTIB past end of comment )

  while (TRUE)
    {
      while ((pTIB < (TIB + 255)) && (! (*pTIB == ')')) && *pTIB) 
	{
	  *pOutStream << *pTIB;
	  ++pTIB; 
	}

      if (*pTIB == ')')
	{
	  pOutStream->flush();
	  ++pTIB;
	  break;
	}
      else
	{
	  *pOutStream << endl;
	  pInStream->getline(TIB, 255);
	  if (pInStream->fail()) return E_V_NO_EOS;
	  ++linecount;
	  pTIB = TIB;
	}
    }

  return 0;
}
//---------------------------------------------------------------

int CPP_dot ()
{
  // stack: ( n -- | print n in current base ) 

  DROP
  if (GlobalSp > BottomOfStack) 
    return E_V_STK_UNDERFLOW;
  else
    {
      long int n = TOS;
      if (n < 0)
	{
	  *pOutStream << '-';
	  TOS = abs(n);
	}
      DEC_DSP
      DEC_DTSP
      return CPP_udot();
    }
  return 0;
}
//--------------------------------------------------------------

int CPP_dotr ()
{
  // stack: ( n1 n2 -- | print n1 in field n2 wide )

  DROP
  if (GlobalSp > BottomOfStack) return E_V_STK_UNDERFLOW;
  
  long int i, n, ndig, nfield;
  long unsigned int u, utemp, uscale;

  nfield = TOS;
  DROP

  if (GlobalSp > BottomOfStack) return E_V_STK_UNDERFLOW;

  if (nfield <= 0) return 0;  // don't print anything if field with <= 0

  n = TOS;
  u = abs(n);
  ndig = 1;
  uscale = 1;
  utemp = u;

  while (utemp /= Base) {++ndig; uscale *= Base;}
  int ntot = (n < 0) ? ndig + 1 : ndig;

  if (ntot <= nfield)
    {
      for (i = 0; i < (nfield - ntot); i++) *pOutStream << ' ';
    }

  if (n < 0) *pOutStream << '-';
  TOS = u;
  DEC_DSP
  DEC_DTSP

  i = CPP_udot0();
  pOutStream->flush();
  return i;
}
//---------------------------------------------------------------

int CPP_udotr ()
{
  // stack: ( u n -- | print unsigned in field width n )

  DROP
  if (GlobalSp > BottomOfStack) return E_V_STK_UNDERFLOW;
  
  long int i, ndig, nfield;
  unsigned long int u, utemp, uscale;

  nfield = TOS;
  DROP
  if (GlobalSp > BottomOfStack) return E_V_STK_UNDERFLOW;

  if (nfield <= 0) return 0;  // don't print anything if field with <= 0

  u = TOS;
  ndig = 1;
  uscale = 1;
  utemp = u;

  while (utemp /= Base) {++ndig; uscale *= Base;}

  if (ndig <= nfield)
    {
      for (i = 0; i < (nfield - ndig); i++) *pOutStream << ' ';
    }
  TOS = u;
  DEC_DSP
  DEC_DTSP

  i = CPP_udot0();
  pOutStream->flush();
  return i;
}
//---------------------------------------------------------------

int CPP_udot0 ()
{
  // stack: ( u -- | print unsigned single in current base )

  DROP
  if (GlobalSp > BottomOfStack) return E_V_STK_UNDERFLOW;
  
  long int i, ndig, nchar;
  unsigned long int u, utemp, uscale;

  u = TOS;
  ndig = 1;
  uscale = 1;
  utemp = u;

  while (utemp /= Base) {++ndig; uscale *= Base;}

  for (i = 0; i < ndig; i++) 
    {
      utemp = u/uscale;
      nchar = (utemp < 10) ? (utemp + 48) : (utemp + 55);
      *pOutStream << (char) nchar;
      u -= utemp*uscale;
      uscale /= Base;
    }
  return 0;
}
//--------------------------------------------------------------

int CPP_udot ()
{
  // stack: ( u -- | print unsigned single in current base followed by space )

  int e = CPP_udot0();
  if (e)
    return e;
  else
    {
      *pOutStream << ' ';
      pOutStream->flush();
    }
  return 0;
}
//---------------------------------------------------------------

int CPP_uddot ()
{
  // stack: ( ud -- | print unsigned double in current base )

  if ((GlobalSp + 2) > BottomOfStack) return E_V_STK_UNDERFLOW;
  
  unsigned long int u1;

  u1 = *(GlobalSp + 1);
  if (u1 == 0)
    {
      DROP
      return CPP_udot();
    }
  else
    {
      C_bracketsharp();
      C_sharps();
      C_sharpbracket();
      CPP_type();
      *pOutStream << ' ';
      pOutStream->flush();
    }
  
  return 0;
}
//---------------------------------------------------------------

int CPP_ddot ()
{
  // stack: ( d -- | print signed double length number )

  if ((GlobalSp + 2) > BottomOfStack) 
    return E_V_STK_UNDERFLOW;
  else
    {
      long int n = *(GlobalSp+1);
      if (n < 0)
	{
	  *pOutStream << '-';
	  L_dabs();
	}
      return CPP_uddot();
    }
  return 0;
}
//---------------------------------------------------------------

int CPP_fdot ()
{
  // stack: ( f -- | print floating point number )

  DROP
  DROP
  if (GlobalSp > BottomOfStack)
    return E_V_STK_UNDERFLOW;
  else
    {
      DEC_DSP
      *pOutStream << *((double*) GlobalSp) << ' ';
      INC_DSP
      (*pOutStream).flush();
    }
  return 0;
}
//---------------------------------------------------------------

int CPP_fsdot ()
{
  // stack: ( f -- | print floating point number )

  DROP
  DROP
  if (GlobalSp > BottomOfStack)
    return E_V_STK_UNDERFLOW;
  else
    {
      DEC_DSP
      ios_base::fmtflags origFlags = cout.flags();
      int origPrec = cout.precision();
      *pOutStream << setprecision(Precision-1) << scientific << 
		*((double*) GlobalSp) << ' ';
      INC_DSP
      (*pOutStream).flush();
      cout.flags(origFlags);
      cout.precision(origPrec);
    }
  return 0;
}
//---------------------------------------------------------------

int CPP_dots ()
{
  if (GlobalSp > BottomOfStack) return E_V_STK_UNDERFLOW;

  L_depth();  
  DROP
  long int depth = TOS;
  DROP

  if (debug)
    {
      *pOutStream << "\nTop of Stack = " << ((long int)ForthStack);
      *pOutStream << "\nBottom of Stack = " << ((long int)BottomOfStack);
      *pOutStream << "\nStack ptr = " << ((long int)GlobalSp);
      *pOutStream << "\nDepth = " << depth;
    }
 
  if (depth > 0)
    {
      int i;
      byte* bptr;

      for (i = 0; i < depth; i++)
        {
#ifndef __FAST__
	  if (*(GlobalTp + i) == OP_ADDR)
            {
                bptr = *((byte**) (GlobalSp + i));
                *pOutStream << "\n\taddr\t" << ((long int)bptr);
            }
            else
            {
                *pOutStream << "\n\t\t" << *(GlobalSp + i);
            }
#else
	  *pOutStream << "\n\t\t" << *(GlobalSp + i);
#endif
        }
    }
  else
    {
        *pOutStream << "<empty>";
    }
  *pOutStream << '\n';
  DEC_DSP
  DEC_DTSP

  return 0;
}
//---------------------------------------------------------------

int CPP_tick ()
{
    // stack: ( "name" -- xt )
    // Return error if "name" is not found in current search order

    char name[128];
    pTIB = ExtractName(pTIB, name);
    strupr(name);
    WordListEntry w;
    if ( SearchOrder.LocateWord (name, &w) )
    {
        PUSH_ADDR((long int) w.Cfa)
    }
    else
	return E_C_UNKNOWNWORD;
    
    return 0;
}

int CPP_defined ()
{
   // stack: ( "name" -- flag)
   int not_found = CPP_tick();
   if ( ! not_found ) { DROP }
   PUSH_IVAL(not_found ? FALSE : TRUE)
   return 0;
}

int CPP_undefined ()
{
   // stack: ( "name" -- flag)
   int not_found = CPP_tick();
   if ( ! not_found ) { DROP }
   PUSH_IVAL(not_found ? TRUE : FALSE)
   return 0;
}

int CPP_find ()
{
  // stack: ( ^str -- ^str 0 | xt_addr 1 | xt_addr -1 )

  DROP
  CHK_ADDR
  unsigned char* s = *((unsigned char**) GlobalSp);
  char name [128];
  int len = *s;
  strncpy (name, (char*) s+1, len);
  name[len] = 0;
  strupr(name);
  WordListEntry w;
  int found = SearchOrder.LocateWord (name, &w);  
  if (found)
    {
      PUSH_ADDR((long int) w.Cfa)
      PUSH_IVAL( (w.Precedence & PRECEDENCE_IMMEDIATE) ? 1 : -1 )
    }
  else
    {
      DEC_DSP
      DEC_DTSP
      PUSH_IVAL(0)
    }
  return 0;
}
//---------------------------------------------------------------

int CPP_emit ()
{
  // stack: ( n -- | display character with ascii code n )

  DROP

  if (GlobalSp > BottomOfStack)
    return E_V_STK_UNDERFLOW;
  else
    {
      *pOutStream << (char) TOS;
      (*pOutStream).flush();
    }
  return 0;
}
//---------------------------------------------------------------

int CPP_cr ()
{
  *pOutStream << '\n';
  return 0;
}
//---------------------------------------------------------------

int CPP_spaces ()
{

  DROP
  if (GlobalSp > BottomOfStack) 
    return E_V_STK_UNDERFLOW;
  else
    {
      int n = TOS;
      if (n > 0)
	for (int i = 0; i < n; i++) *pOutStream << ' ';
      (*pOutStream).flush();
    }
  return 0;
}
//---------------------------------------------------------------

int CPP_type ()
{

  DROP
  if (GlobalSp > BottomOfStack) 
    return E_V_STK_UNDERFLOW;
  else
    {
      int n = TOS;
      DROP
      if (GlobalSp > BottomOfStack) 
	return E_V_STK_UNDERFLOW;
      CHK_ADDR
      char* cp = *((char**) GlobalSp);
      for (int i = 0; i  < n; i++) *pOutStream << *cp++;
      (*pOutStream).flush();
    }
  return 0;
}
//---------------------------------------------------------------

int CPP_words ()
{
  char *cp, field[16];
  int nc;
  Vocabulary* pVoc = SearchOrder.front();
  *pOutStream << pVoc->size() << " words.\n";
  WordIndex i;
  int j = 0;
  for (i = pVoc->begin(); i < pVoc->end(); i++)
    {
      memset (field, 32, 16);
      field[15] = '\0';
      cp = i->WordName;
      nc = strlen(cp);
      strncpy (field, cp, (nc > 15) ? 15 : nc);
      *pOutStream << field;
      if ((++j) % 5 == 0) *pOutStream << '\n';
    }
  return 0;
}
//---------------------------------------------------------------

int CPP_allocate()
{
    // stack: ( u -- a ior | allocate u bytes and return address and success)

  DROP
  if (GlobalSp > BottomOfStack) 
    return E_V_STK_UNDERFLOW;

#ifndef __FAST__
  if (*GlobalTp != OP_IVAL)
    return E_V_NOTIVAL;  // need an int
#endif

  unsigned int requested = TOS;
  byte *p = new (nothrow) byte[requested];
  PUSH_ADDR((long int) p)
  PUSH_IVAL(p ? 0 : -1)
  return 0;
}


int CPP_free()
{
    // stack: ( a -- ior | free the allocated region at address a )
    DROP
    CHK_ADDR
    byte *p = (byte*) TOS; 
    delete [] p;
    PUSH_IVAL(0)
    return 0;
}


int CPP_resize()
{
    // stack: ( a unew -- anew ior )
    DROP
    unsigned long unew = TOS;
    DROP
    void* pOld = (void*) TOS;
    CHK_ADDR
    void* pNew = realloc(pOld, unew);
    if (pNew) TOS = (long int) pNew;
    DEC_DSP
    DEC_DTSP
    TOS = (pNew == NULL); 
    DEC_DSP
    DEC_DTSP

    return 0;
}
//----------------------------------------------------------------

int CPP_allot ()
{
  DROP
  if (GlobalSp > BottomOfStack) 
    return E_V_STK_UNDERFLOW;
#ifndef __FAST__
  if (*GlobalTp != OP_IVAL)
    return E_V_BADTYPE;  // need an int
#endif

  WordIndex id = pCompilationWL->end() - 1;
  long int n = TOS;
  if (n > 0)
    {
      if (id->Pfa == NULL)
	{ 
	  id->Pfa = new byte[n];
	  if (id->Pfa) memset (id->Pfa, 0, n); 

	  // Provide execution code to the word to return its Pfa
  	  byte *bp = new byte[WSIZE+2];
  	  id->Cfa = bp;
  	  bp[0] = OP_ADDR;
  	  *((long int*) &bp[1]) = (long int) id->Pfa;
  	  bp[WSIZE+1] = OP_RET;
	}
      else 
	return E_V_REALLOT;
    }
  else
    id->Pfa = NULL;

  return 0;
}
//--------------------------------------------------------------

int CPP_queryallot ()
{
  // stack: ( n -- a | allot n bytes and leave starting address on the stack )

  int e = CPP_allot();
  if (!e)
    {
      // Get last word's Pfa and leave on the stack

      WordIndex id = pCompilationWL->end() - 1;
      PUSH_ADDR((long int) id->Pfa)
    }
  return e;
}
//---------------------------------------------------------------

int CPP_create ()
{

  // stack: ( -- | create dictionary entry using next word in input stream )

  char token[128];
  pTIB = ExtractName(pTIB, token);
  int nc = strlen(token);

  if (nc)
    {
      WordListEntry NewWord;
      strupr(token);
      strcpy (NewWord.WordName, token);
      NewWord.WordCode = OP_ADDR;
      NewWord.Pfa = NULL;
      NewWord.Cfa = NULL;
      NewWord.Precedence = 0;

      pCompilationWL->push_back(NewWord);
      return 0;
    }
  else
    {
      return E_V_CREATE;  // create failed
    }
}
//-----------------------------------------------------------------

int CPP_alias ()
{
    // stack: ( xt "name" -- )
    DROP
    void* cfa = (void*) TOS;
    CHK_ADDR
    WordListEntry w;
    bool found = SearchOrder.LocateCfa(cfa, &w);
    if (found) {
      CPP_create();
      WordIndex j = pCompilationWL->end() - 1;
      byte* bp = new byte[WSIZE+2];
      j->Cfa = bp;
      j->Pfa = NULL;
      j->Precedence = w.Precedence;
      j->WordCode = OP_DEFINITION;
      bp[0] = OP_DEFINITION;
      *((long int*)(bp+1)) = (long int) w.Cfa;
      bp[WSIZE+1] = OP_RET;
    }
    else
      return E_C_UNKNOWNWORD;

    return 0;
}
//-----------------------------------------------------------------

int CPP_variable ()
{
  // stack: ( -- | create dictionary entry and allot space )

  if (CPP_create()) return E_V_CREATE;  
  TOS = sizeof(long int);
  DEC_DSP
  STD_IVAL
  int e = CPP_allot();
  return e;
}
//-----------------------------------------------------------------

int CPP_fvariable ()
{
  // stack: ( -- | create dictionary entry and allot space )

  if (CPP_create()) return E_V_CREATE;  
  TOS = sizeof(double);
  DEC_DSP
  STD_IVAL
  int e = CPP_allot();
  return e;
}
//------------------------------------------------------------------

int CPP_constant ()
{
  // stack: ( n -- | create dictionary entry and store n as constant )

  if (CPP_create()) return E_V_CREATE;
  WordIndex id = pCompilationWL->end() - 1;
  DROP
  id->WordCode = IS_ADDR ? OP_PTR : OP_IVAL;
  id->Pfa = new long int[1];
  *((long int*) (id->Pfa)) = TOS;
  byte *bp = new byte[WSIZE+3];
  id->Cfa = bp;
  bp[0] = OP_ADDR;
  *((long int*) &bp[1]) = (long int) id->Pfa;
  bp[WSIZE+1] = (id->WordCode == OP_PTR) ? OP_AFETCH : OP_FETCH;
  bp[WSIZE+2] = OP_RET;
  return 0;
}
//------------------------------------------------------------------

int CPP_fconstant ()
{
  // stack: ( f -- | create dictionary entry and store f )

  if (CPP_create()) return E_V_CREATE;
  WordIndex id = pCompilationWL->end() - 1;
  id->WordCode = OP_FVAL;
  id->Pfa = new double[1];
  DROP
  *((double*) (id->Pfa)) = *((double*)GlobalSp);
  DROP
  byte *bp = new byte[WSIZE+3];
  id->Cfa = bp;
  bp[0] = OP_ADDR;
  *((long int*) &bp[1]) = (long int) id->Pfa;
  bp[WSIZE+1] = OP_DFFETCH;
  bp[WSIZE+2] = OP_RET;
  return 0;
}
//------------------------------------------------------------------

int CPP_char ()
{
  // stack: ( -- n | parse next word in input stream and return first char )

  PUSH_IVAL(32)
  C_word();
  char* cp = *((char**) ++GlobalSp) + 1;
  *GlobalSp-- = *cp;
#ifndef __FAST__
  *(GlobalTp + 1) = OP_IVAL ;
#endif
  return 0;
}
//-----------------------------------------------------------------

int CPP_bracketchar ()
{
  CPP_char();
  CPP_literal();
  return 0;
}
//------------------------------------------------------------------

int CPP_brackettick ()
{
  CPP_tick ();
  return CPP_literal();  
}
//-------------------------------------------------------------------

int CPP_compilecomma ()
{
  // stack: xt --
  if (State == 0) pCurrentOps = pPreviousOps;
  CPP_literal();
  pCurrentOps->push_back(OP_EXECUTE);
  if (State == 0) pCurrentOps = &tempOps;
  return 0;  
}
// -----------------------------------------------------------------

int CPP_bracketcompile ()
{
  return 0;
}
// -----------------------------------------------------------------

int CPP_postpone ()
{
  char token[128];
  byte* bp;

  pTIB = ExtractName (pTIB, token);
  strupr(token);
  WordListEntry w;
  int found = SearchOrder.LocateWord(token, &w);
  if (found) {
      if (w.Precedence & PRECEDENCE_IMMEDIATE)
	{
	  CompileWord(w);
	}
      else
	{
	  int wc = (w.WordCode >> 8) ? OP_CALLADDR : w.WordCode;

	  if (wc == OP_IVAL)
	    {
	      pCurrentOps->push_back(OP_IVAL);
	      OpsPushInt (*((long int*) w.Pfa));
	      pCurrentOps->push_back(OP_LITERAL);
	    }
	  else if ((wc == OP_ADDR) || (wc == OP_PTR))
	    {
	      pCurrentOps->push_back(wc);
	      OpsPushInt ((long int) w.Pfa);
	      pCurrentOps->push_back(OP_LITERAL);
	    }
	  else
	    {
	      pCurrentOps->push_back(OP_IVAL);
	      OpsPushInt(wc);
	      pCurrentOps->push_back(OP_CALLADDR);
	      OpsPushInt((long int) OpsCompileByte);

	      switch (wc)
		{
		case OP_DEFINITION:
		  pCurrentOps->push_back(OP_ADDR);
		  OpsPushInt((long int) w.Cfa);
		  pCurrentOps->push_back(OP_CALLADDR);
		  OpsPushInt((long int) OpsCompileInt);
		  break;
		case OP_CALLADDR:
		  pCurrentOps->push_back(OP_ADDR);
		  bp = (byte*) w.Cfa; ++bp;
		  OpsPushInt(*((long int*)bp));
		  pCurrentOps->push_back(OP_CALLADDR);
		  OpsPushInt((long int) OpsCompileInt);
		  break;
		case OP_FVAL:
	          pCurrentOps->push_back(OP_FVAL);
	          OpsPushDouble (*((double*) w.Pfa));
	          pCurrentOps->push_back(OP_CALLADDR);
	          OpsPushInt((long int) OpsCompileDouble);
		  break;
		default:
		  ;
		}
	    } 
	}
      if (State && (w.Precedence & PRECEDENCE_NON_DEFERRED)) 
	NewWord.Precedence |= PRECEDENCE_NON_DEFERRED;

    }


  return 0;
  
}
// -----------------------------------------------------------------

int CPP_forget ()
{
  char token[128];

  pTIB = ExtractName (pTIB, token);
  strupr(token);

  WordIndex i = pCompilationWL->IndexOf( token );
  WordIndex j = pCompilationWL->end();
  if ( i < j )
    {
       while (j > i) {
         --j;
         pCompilationWL->RemoveLastWord();
       }
    }
  else
    {
      *pOutStream << "No such word in the current wordlist: " << token << '\n';
    }
  return 0;
}
//-------------------------------------------------------------------

int CPP_cold ()
{
  // stack: ( -- | restart the Forth environment )

  CloseForth();
  OpenForth();

  return 0;
}
//--------------------------------------------------------------------

int CPP_bye ()
{
  // stack: ( -- | close Forth and exit the process )

  CloseForth();
  *pOutStream << "Goodbye.\n";
  exit(0);

  return 0;
}
//--------------------------------------------------------------------

int CPP_tofile ()
{
  char filename[128];
  *filename = 0;

  pTIB = ExtractName (pTIB, filename);
  if (*filename == 0)
    {
      strcpy (filename, DEFAULT_OUTPUT_FILENAME);
      // cout << "Output redirected to " << filename << '\n';
    }
  ofstream *pFile = new ofstream (filename);
  if (! pFile->fail())
    {
      if (FileOutput)
	{ 
	  (*((ofstream*) pOutStream)).close();  // close current file output stream
	  delete pOutStream;
	} 
      pOutStream = pFile;
      FileOutput = TRUE;
    }
  else
    {
      *pOutStream << "Failed to open output file stream.\n";
    }
  return 0;  
}
//--------------------------------------------------------------------

int CPP_console ()
{
  if (FileOutput)
    {
      (*((ofstream*) pOutStream)).close();  // close the current file output stream
      delete pOutStream;
    }     
  pOutStream = &cout;  // make console the new output stream
  FileOutput = FALSE;

  return 0;
}
//--------------------------------------------------------------------

int CPP_literal ()
{
  // stack: ( n -- | remove item from the stack and place in compiled opcodes )
  DROP
#ifndef __FAST__
  pCurrentOps->push_back(*GlobalTp);
#else
  pCurrentOps->push_back(OP_IVAL);
#endif
  OpsPushInt(TOS);  
  return 0;
}
//-------------------------------------------------------------------

int CPP_twoliteral ()
{
  // stack: ( n1 n2 -- | remove items from the stack and place in compiled opcodes )

  GlobalSp += 2;
#ifndef __FAST__
  GlobalTp += 2;
  pCurrentOps->push_back(*GlobalTp);
#else
  pCurrentOps->push_back(OP_IVAL);
#endif

  OpsPushInt(TOS);
#ifndef __FAST__
  pCurrentOps->push_back(*(GlobalTp - 1));
#else
  pCurrentOps->push_back(OP_IVAL);
#endif
  OpsPushInt(*(GlobalSp - 1));  
  return 0;
}
//-------------------------------------------------------------------

int CPP_sliteral ()
{
  // stack: ( c-addr u -- | place string or copy of string in compiled opcodes )
  DROP
  unsigned long int u = TOS;
  DROP
  CHK_ADDR
  char *cp = (char*) TOS;
  pCurrentOps->push_back(OP_ADDR);
  // If string is not already in the string table, put it there
  if (! InStringTable(cp-1)) 
  {
    char* str = new char[u + 1];
    strncpy(str, cp, u);
    str[u] = '\0';
    StringTable.push_back(str);
    cp = str;
  }
  OpsPushInt((long int) cp);
  pCurrentOps->push_back(OP_IVAL);
  OpsPushInt(u);

  return 0;
}
//-------------------------------------------------------------------

int CPP_cquote ()
{
  // compilation stack: ( -- | compile a counted string into the string table )
  // runtime stack: ( -- ^str | place address of counted string on stack )

  char* begin_string = pTIB;
  char* end_string = strchr(begin_string, '"');
  if (end_string == NULL)
    {
      return E_V_NO_EOS;
    }
  pTIB = end_string + 1;
  int nc = (int) (end_string - begin_string);
  char* str = new char[nc + 2];
  *((byte*)str) = (byte) nc;
  strncpy(str+1, begin_string, nc);
  str[nc+1] = '\0';
  StringTable.push_back(str);
  pCurrentOps->push_back(OP_ADDR);
  OpsPushInt((long int) str);

  return 0;
}
//-------------------------------------------------------------------

int CPP_squote ()
{
  // compilation stack: ( -- | compile a string into the string table )
  // runtime stack: ( -- a count )

  int e = CPP_cquote();
  if (e) return e;
  char* s = *(StringTable.end() - 1);
  long int v = (byte) s[0];
  pCurrentOps->push_back(OP_INC);
  pCurrentOps->push_back(OP_IVAL);
  OpsPushInt(v);

  return 0;
}
//-------------------------------------------------------------------

int CPP_dotquote ()
{
  // stack: ( -- | display a string delimited by quote from the input stream)

  int e = CPP_cquote();
  if (e) return e;

  pCurrentOps->push_back(OP_COUNT);
  pCurrentOps->push_back(OP_TYPE);

  return 0;
}
//------------------------------------------------------------------

int CPP_do ()
{
  // stack: ( -- | generate opcodes for beginning of loop structure )

  pCurrentOps->push_back(OP_PUSH);
  pCurrentOps->push_back(OP_PUSH);
  pCurrentOps->push_back(OP_PUSHIP);

  dostack.push_back(pCurrentOps->size());
  return 0;
}
//------------------------------------------------------------------

int CPP_querydo ()
{
  // stack: ( -- | generate opcodes for beginning of conditional loop )
  
  pCurrentOps->push_back(OP_2DUP);
  pCurrentOps->push_back(OP_EQ);
  CPP_if();
  pCurrentOps->push_back(OP_2DROP);
  CPP_else();
  CPP_do();

  querydostack.push_back(pCurrentOps->size());
  return 0;
}
//------------------------------------------------------------------

int CPP_leave ()
{
  // stack: ( -- | generate opcodes to jump out of the current loop )

  if (dostack.empty()) return E_V_NO_DO;
  pCurrentOps->push_back(OP_UNLOOP);
  pCurrentOps->push_back(OP_JMP);
  leavestack.push_back(pCurrentOps->size());
  OpsPushInt(0);
  return 0;
}
//------------------------------------------------------------------

int CPP_abortquote ()
{
  // stack: ( -- | generate opcodes to print message and abort )

  long int nc = strlen(NewWord.WordName);;
  char* str = new char[nc + 3];
  strcpy(str, NewWord.WordName);
  strcat(str, ": ");
  StringTable.push_back(str);

  pCurrentOps->push_back(OP_JZ);
  OpsPushInt(4*WSIZE+9);   // relative jump count                       

// the relative jump count (above) must be modified if the 
// instructions below are updated!

  pCurrentOps->push_back(OP_ADDR);
  OpsPushInt((long int) str);
  pCurrentOps->push_back(OP_IVAL);
  OpsPushInt(nc+2);
  pCurrentOps->push_back(OP_TYPE);
  int e = CPP_dotquote();
  pCurrentOps->push_back(OP_CR);
  pCurrentOps->push_back(OP_ABORT);
  return e;

}
//------------------------------------------------------------------

int CPP_begin()
{
  // stack: ( -- | mark the start of a begin ... structure )

  beginstack.push_back(pCurrentOps->size());
  return 0;
}
//------------------------------------------------------------------

int CPP_while()
{
  // stack: ( -- | build the begin ... while ... repeat structure )	      

  if (beginstack.empty()) return E_V_NO_BEGIN;
  pCurrentOps->push_back(OP_JZ);
  whilestack.push_back(pCurrentOps->size());
  OpsPushInt(0);
  return 0;
}
//------------------------------------------------------------------

int CPP_repeat()
{
  // stack: ( -- | complete begin ... while ... repeat block )

  if (beginstack.empty()) return E_V_NO_BEGIN;  // no matching BEGIN

  int i = beginstack[beginstack.size()-1];
  beginstack.pop_back();

  long int ival;

  if (whilestack.size())
    {
      int j = whilestack[whilestack.size()-1];
      if (j > i)
	{
	  whilestack.pop_back();
	  ival = pCurrentOps->size() - j + WSIZE + 2;
	  OpsCopyInt (j, ival);  // write the relative jump count
	}
    }

  ival = i - pCurrentOps->size();
  pCurrentOps->push_back(OP_JMP);
  OpsPushInt(ival);   // write the relative jump count

  return 0;
}
//-------------------------------------------------------------------

int CPP_until()
{
  // stack: ( -- | complete begin ... until block )

  if (beginstack.empty()) return E_V_NO_BEGIN;  // no matching BEGIN

  int i = beginstack[beginstack.size()-1];
  beginstack.pop_back();
  long int ival = i - pCurrentOps->size();
  pCurrentOps->push_back(OP_JZ);
  OpsPushInt(ival);   // write the relative jump count

  return 0;
}
//-------------------------------------------------------------------

int CPP_again()
{
  // stack: ( -- | complete begin ... again block )

  if (beginstack.empty()) return E_V_NO_BEGIN;  // no matching BEGIN

  int i = beginstack[beginstack.size()-1];
  beginstack.pop_back();
  long int ival = i - pCurrentOps->size();
  pCurrentOps->push_back(OP_JMP);
  OpsPushInt(ival);   // write the relative jump count

  return 0;
}
//--------------------------------------------------------------------

int CPP_if()
{
  // stack: ( -- | generate start of an if-then or if-else-then block )

  pCurrentOps->push_back(OP_JZ);
  ifstack.push_back(pCurrentOps->size());
  OpsPushInt(0);   // placeholder for jump count
  return 0;
}
//------------------------------------------------------------------

int CPP_else()
{
  // stack: ( -- | build the if-else-then block )

  pCurrentOps->push_back(OP_JMP);
  OpsPushInt(0);  // placeholder for jump count

  if (ifstack.empty()) return E_V_ELSE_NO_IF;  // ELSE without matching IF
  int i = ifstack[ifstack.size()-1];
  ifstack.pop_back();
  ifstack.push_back(pCurrentOps->size() - sizeof(long int));
  long int ival = pCurrentOps->size() - i + 1;
  OpsCopyInt (i, ival);  // write the relative jump count

  return 0;
}
//-------------------------------------------------------------------

int CPP_then()
{
  // stack: ( -- | complete the if-then or if-else-then block )

  if (ifstack.empty()) 
    return E_V_THEN_NO_IF;  // THEN without matching IF or IF-ELSE

  int i = ifstack[ifstack.size()-1];
  ifstack.pop_back();
  long int ival = (long int) (pCurrentOps->size() - i) + 1;
  OpsCopyInt (i, ival);   // write the relative jump count

  return 0;
}
//-------------------------------------------------------------------

int CPP_case()
{
  // stack: ( n -- | mark the beginning of a case...endcase structure)

  casestack.push_back(-1);
  return 0;
}
//-----------------------------------------------------------------

int CPP_endcase()
{
  // stack: ( -- | terminate the case...endcase structure)

  if (casestack.size() == 0) return E_V_NO_CASE;  // ENDCASE without matching CASE
  pCurrentOps->push_back(OP_DROP);

  // fix up all absolute jumps

  int i; long int ival;
  do
    {
      i = casestack[casestack.size()-1];
      casestack.pop_back();
      if (i == -1) break;
      ival = (long int) (pCurrentOps->size() - i) + 1;
      OpsCopyInt (i, ival);   // write the relative jump count
    } while (casestack.size()) ;

  return 0;
}
//----------------------------------------------------------------

int CPP_of()
{
  // stack: ( -- | generate start of an of...endof block)

  pCurrentOps->push_back(OP_OVER);
  pCurrentOps->push_back(OP_EQ);
  pCurrentOps->push_back(OP_JZ);
  ofstack.push_back(pCurrentOps->size());
  OpsPushInt(0);   // placeholder for jump count
  pCurrentOps->push_back(OP_DROP);
  return 0;
}
//-----------------------------------------------------------------

int CPP_endof()
{
  // stack: ( -- | complete an of...endof block)

  pCurrentOps->push_back(OP_JMP);
  casestack.push_back(pCurrentOps->size());
  OpsPushInt(0);   // placeholder for jump count

  if (ofstack.empty())
    return E_V_ENDOF_NO_OF;  // ENDOF without matching OF

  int i = ofstack[ofstack.size()-1];
  ofstack.pop_back();
  long int ival = (long int) (pCurrentOps->size() - i) + 1;
  OpsCopyInt (i, ival);   // write the relative jump count

  return 0;
}
//-----------------------------------------------------------------

int CPP_recurse()
{
  pCurrentOps->push_back(OP_ADDR);
  if (State)
    {
      recursestack.push_back(pCurrentOps->size());
      OpsPushInt(0);
    }
  else
    {
      long int ival = (long int) &(*pCurrentOps)[0]; // ->begin();
      OpsPushInt(ival);
    }
  pCurrentOps->push_back(OP_EXECUTE);
  return 0;
}
//---------------------------------------------------------------------

int CPP_lbracket()
{
  State = FALSE;
  pPreviousOps = pCurrentOps;
  tempOps.erase(tempOps.begin(), tempOps.end());
  pCurrentOps = &tempOps;
  return 0;
}
//--------------------------------------------------------------------

int CPP_rbracket()
{
  pCurrentOps->push_back(OP_RET);
  if (debug) OutputForthByteCode(pCurrentOps);
  byte* pIp = GlobalIp;
  int e = vm((byte*) &(*pCurrentOps)[0]);
  pCurrentOps->erase(pCurrentOps->begin(), pCurrentOps->end());
  GlobalIp = pIp;
  State = TRUE;
  pCurrentOps = pPreviousOps;
  return e;
}
//-------------------------------------------------------------------

int CPP_does()
{
  // Allocate new opcode array

  byte* p = new byte[2*WSIZE+4];

  // Insert pfa of last word in dictionary

  p[0] = OP_ADDR;
  WordIndex id = pCompilationWL->end() - 1;
  *((long int*)(p+1)) = (long int) id->Pfa;

  // Insert current instruction ptr 

  p[WSIZE+1] = OP_ADDR;
  *((long int*)(p+WSIZE+2)) = (long int)(GlobalIp + 1);

  p[2*WSIZE+2] = OP_EXECUTE;
  p[2*WSIZE+3] = OP_RET;

  id->Cfa = (void*) p;
  id->WordCode = OP_DEFINITION;

  L_ret();
  return 0;
}
//-------------------------------------------------------------------

int CPP_immediate ()
{
  // Mark the most recently defined word as immediate.
  // stack: ( -- )

  WordIndex id = pCompilationWL->end() - 1;
  id->Precedence |= PRECEDENCE_IMMEDIATE;
  return 0;
}
//-------------------------------------------------------------------

int CPP_nondeferred ()
{
  // Mark the most recently defined word as non-deferred.
  // stack: ( -- )

  WordIndex id = pCompilationWL->end() - 1;
  id->Precedence |= PRECEDENCE_NON_DEFERRED;
  return 0;
}
//-------------------------------------------------------------------

int CPP_evaluate ()
{
  // Evaluate a Forth source string
  // ( ... a u -- ? )

  char s[256], s2[256];
  DROP
  long int nc = TOS; int ec = 0;
  DROP
  char *cp = (char*) TOS;
  if (nc < 256)
    {
      memcpy (s, cp, nc);
      s[nc] = 0;
      if (*s) 
	{
	  istringstream* pSS = NULL;
	  istream* pOldStream = pInStream;  // save old input stream
	  strcpy (s2, pTIB);  // save remaining part of input line in TIB
	  pSS = new istringstream(s);
	  SetForthInputStream(*pSS);
	  vector<byte> op, *pOps, *pOldOps;
	  
	  pOldOps = pCurrentOps;
	  pOps = State ? pCurrentOps : &op;

	  --linecount;
	  ec = ForthCompiler(pOps, &linecount);
	  if ( State && (ec == E_C_ENDOFSTREAM)) ec = 0;

	  // Restore the opcode vector, the input stream, and the input buffer

	  pCurrentOps = pOldOps;
	  SetForthInputStream(*pOldStream);  // restore old input stream
	  strcpy(TIB, s2);  // restore TIB with remaining input line
	  pTIB = TIB;      // restore ptr
	  delete pSS;

	}
    }
  return( ec );
  
}
//-------------------------------------------------------------------

int CPP_included()
{
  // include the filename given on the stack as a counted string
  // ( ... a u -- ?)

  char filename[256];
  DROP
  long int nc = TOS;
  DROP
  char *cp = (char*) TOS;

  if ((nc < 0) || (nc > 255)) return E_V_OPENFILE;

  memcpy (filename, cp, nc);
  filename[nc] = 0;
  if (!strchr(filename, '.')) strcat(filename, ".4th");

  ifstream f(filename);
  if (!f)
    {
      if (getenv(dir_env_var))
	{
	  char temp[256]; 
	  strcpy(temp, getenv(dir_env_var));
	  strcat(temp, "/");
	  strcat(temp, filename);
	  strcpy(filename, temp);
	  f.clear();                // Clear the previous error.
	  f.open(filename);
	  if (f) 
	    {
	      *pOutStream << endl << filename << endl;
	    }
	}
    }

  if (f.fail()) 
    {
      *pOutStream << endl << filename << endl;
      return (E_V_OPENFILE);
    }

  vector<byte> ops, *pOldOps;
  int ecode;
	
  istream* pTempIn = pInStream;  // save input stream ptr
  SetForthInputStream(f);  // set the new input stream
  long int oldlc = linecount; linecount = 0;
  pOldOps = pCurrentOps;
  ecode = ForthCompiler (&ops, &linecount);
  f.close();
  pInStream = pTempIn;  // restore the input stream
  pCurrentOps = pOldOps; 
  if (ecode) 
    {
      *pOutStream << filename << "  " ;
      return (ecode);
    }
  linecount = oldlc;

  // Execute the code immediately
		      
  long int *sp;
  byte *tp;
  ecode = ForthVM (&ops, &sp, &tp);
  ops.erase(ops.begin(), ops.end());

  return ecode;
}

//-------------------------------------------------------------------

int CPP_include()
{
    char WordToken[256], s[256];
    int ecode;

    pTIB = ExtractName (pTIB, WordToken);
    strcpy (s, pTIB);  // save remaining part of input line in TIB

    PUSH_ADDR((long int) ((char*) WordToken))
    PUSH_IVAL(strlen(WordToken))
    ecode = CPP_included();
    if (ecode) return(ecode);

    strcpy(TIB, s);  // restore TIB with remaining input line
    pTIB = TIB;      // restore ptr
    
    return 0;
}
//-------------------------------------------------------------------

int CPP_source()
{
    PUSH_ADDR((long int) TIB)
    PUSH_IVAL(strlen(TIB))
    return 0;
}
//-------------------------------------------------------------------

int CPP_refill()
{
    pInStream->getline(TIB, 255); 
    long int flag = (pInStream->fail()) ? FALSE : TRUE;
    if (flag) ++linecount;
    PUSH_IVAL(flag)
    pTIB = TIB;
    return 0;
}
//-------------------------------------------------------------------

int CPP_state()
{
    PUSH_ADDR((long int)(&State))
    return 0;
}
//-------------------------------------------------------------------

int CPP_spstore()
{
    // stack: ( addr -- | make the stack ptr point to a new address)

// cout << "GlobalSp = " << GlobalSp << "  ForthStack = " << ForthStack << endl;
// #ifndef __FAST__
// cout << "GlobalTp = " << (void *) GlobalTp << "  ForthTypeStack = " << (void *) ForthTypeStack << endl;
// #endif

    DROP
    CHK_ADDR
    long int* p = (long int*) TOS; --p;
    if ((p > BottomOfStack) || (p < ForthStack))
	return E_V_BADSTACKADDR;  // new SP must be within its stack space
    int n = (int) (p - ForthStack);

    GlobalSp = ForthStack + n;
#ifndef __FAST__
    GlobalTp = (byte *) ForthTypeStack + n;
#endif
    return 0;
}
//--------------------------------------------------------------------

int CPP_rpstore()
{
    // stack: ( addr -- | make the stack ptr point to a new address)

    DROP
    CHK_ADDR
    long int* p = (long int*) TOS; --p;
    if ((p > BottomOfReturnStack) || (p < ForthReturnStack))
	return E_V_BADSTACKADDR;  // new RP must be within its stack space

    int n = (int) (p - ForthReturnStack);
    GlobalRp = ForthReturnStack + n;
#ifndef __FAST__
    GlobalRtp = ForthReturnTypeStack + n;
#endif
    return 0;
}

void dump_return_stack()  // for debugging purposes
{
    long int* p = GlobalRp;
    cout << endl << "Return Stack: " << endl;
    while (p < BottomOfReturnStack)
    {
	++p; 
	cout << (int)*p << endl; 
    }
}
}
