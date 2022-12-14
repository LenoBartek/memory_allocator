/*
 *
 *  RLDebugger - Resource Leakage Debugger
 *  Autor: Tomasz Jaworski, 2018-2020
 *
 */

#define _RLDEBUG_IMPLEMENTATION_
#define _RLDEBUG_API_

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include <errno.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>


#include "rdebug.h"


#define RLD_MAGIC1			0xc8fccdb505c6ac13	
#define RLD_MAGIC2			0xece706d3d0533953

#define ALIGN(n, __type)	(n & ~(sizeof(__type)-1)) + sizeof(__type)*!!(n & (sizeof(__type)-1))
#define MIN(__x, __y) ((__x) < (__y) ? (__x) : (__y))
#define MAX(__x, __y) ((__x) > (__y) ? (__x) : (__y))

#define IS_ALLOCATION_FUNCTION_CODE(__code) ((__code) == HFC_CALLOC || (__code) == HFC_MALLOC || (__code) == HFC_REALLOC || (__code) == HFC_STRDUP || (__code) == HFC_STRNDUP)

#if defined(_HTML_OUTPUT)
    #define BOLD(str) "<b>" str "</b>"
    #define BYELLOW(str) "<span style=\"background-color:yellow\">" str "</span>"
    #define BOLDGREEN(str) "<span style=\"color:green; font-weight:bold\">" str "</span>"
    #define BOLDRED(str) "<span style=\"color:red;font-weight:bold\">" str "</span>"
    #define BOLDPINK(str) "<span style=\"color:magenta;font-weight:bold\">" str "</span>"
#endif

#if defined(_ANSI_OUTPUT)
    #define BOLD(str) "\x1b[1m" str "\x1b[0m"
    #define YELLOW(str) "\x1b[33m" str "\x1b[0m"
    #define BOLDGREEN(str) "\x1b[32m" BOLD(str) "\x1b[0m"
    #define BOLDRED(str) "\x1b[31m" BOLD(str) "\x1b[0m"
    #define BOLDPINK(str) "\x1b[35m" BOLD(str) "\x1b[0m"
#endif

#if !defined(_HTML_OUTPUT) && !defined(_ANSI_OUTPUT)
    #define BOLD(str) str
    #define YELLOW(str) str
    #define BOLDGREEN(str) str
    #define BOLDRED(str) str
    #define BOLDPINK(str) str
#endif

enum resource_type_t {
	RT_MEMORY,
	RT_STREAM
};

struct block_fence_t {
	uint8_t pattern[32];
};

struct rld_settings_t {

	//rldebug_callback_t callback;
	enum message_severity_level_t lowest_reported_severity;
	
	// ogranicznik wielko??ci sterty
	struct {
		int global_limit_active;
		size_t global_limit_value;
		int global_disable;
	} heap;
};


enum rld_message_t {
	RLD_UNDEFINED = 0,
	RLD_HEAP_FUNCTIONS_DISABLED,
	
	RLD_MALLOC_NO_MEMORY,
	RLD_MALLOC_NO_MEMORY_DUE_LIMIT,
	RLD_MALLOC_NO_MEMORY_DUE_SINGLESHOT_LIMIT,
	RLD_MALLOC_NO_MEMORY_DUE_CUMULATIVE_LIMIT,
	RLD_MALLOC_FAILED_DUE_SUCCESS_LIMIT,
	RLD_MALLOC_SUCCESSFUL,
	
	RLD_FREE_NULL,
	RLD_FREE_INVALID_POINTER,
	RLD_FREE_SUCCESSFUL,

	RLD_CALLOC_NO_MEMORY,
	RLD_CALLOC_NO_MEMORY_DUE_LIMIT,
	RLD_CALLOC_NO_MEMORY_DUE_SINGLESHOT_LIMIT,
	RLD_CALLOC_NO_MEMORY_DUE_CUMULATIVE_LIMIT,
	RLD_CALLOC_FAILED_DUE_SUCCESS_LIMIT,
	RLD_CALLOC_SUCCESSFUL,
	
	RLD_REALLOC_NO_MEMORY,
	RLD_REALLOC_NO_MEMORY_DUE_LIMIT,
	RLD_REALLOC_NO_MEMORY_DUE_SINGLESHOT_LIMIT,
	RLD_REALLOC_NO_MEMORY_DUE_CUMULATIVE_LIMIT,
	RLD_REALLOC_FAILED_DUE_SUCCESS_LIMIT,
	RLD_REALLOC_INVALID_POINTER,
	RLD_REALLOC_SUCCESSFUL,

	//
	
	RLD_STRDUP_NULL,
	RLD_STRDUP_NO_MEMORY,
	RLD_STRDUP_NO_MEMORY_DUE_LIMIT,
	RLD_STRDUP_NO_MEMORY_DUE_SINGLESHOT_LIMIT,
	RLD_STRDUP_NO_MEMORY_DUE_CUMULATIVE_LIMIT,
	RLD_STRDUP_FAILED_DUE_SUCCESS_LIMIT,
	RLD_STRDUP_SUCCESSFUL,
	
	RLD_STRNDUP_NULL,
	RLD_STRNDUP_NO_MEMORY,
	RLD_STRNDUP_NO_MEMORY_DUE_LIMIT,
	RLD_STRNDUP_NO_MEMORY_DUE_SINGLESHOT_LIMIT,
	RLD_STRNDUP_NO_MEMORY_DUE_CUMULATIVE_LIMIT,
	RLD_STRNDUP_FAILED_DUE_SUCCESS_LIMIT,
	RLD_STRNDUP_SUCCESSFUL,

	//

	RLD_HEAP_BROKEN,
	RLD_HEAP_DATA_OUT_OF_BOUNDS,

	//
	
	RLD_FOPEN_SUCCESSFUL,
	RLD_FOPEN_FAILED,
	RLD_FOPEN_FAILED_DUE_SUCCESS_LIMIT,

	RLD_FCLOSE_NULL_STREAM,
	RLD_FCLOSE_INVALID_STREAM,
	RLD_FCLOSE_SUCCESSFUL,
	
};


struct resource_t {
	uint64_t magic1;
	
	enum resource_type_t type; // typ zasobu
	union {
		
		// blok pamieci
		struct {
			size_t size;
			void* base_pointer;
			enum heap_function_code_t allocated_by;
			
			struct block_fence_t head_fence;
			struct block_fence_t tail_fence;
		} memory;

		// strumien
		struct {
			char* name;
			char* mode;
			FILE* stream;
		} stream;
	};

	// wspolne dane
	const char* source_file;
	int source_line;
	
	struct resource_t *pnext, *pprev; // nast??pny i poprzedni zas??b
	
	uint32_t checksum;
	uint64_t magic2;
};


static struct resource_base_t {
	struct resource_t *phead, *ptail;
	
	size_t current_heap_size;
	size_t top_heap_size;
	
	struct rld_settings_t settings;

	FILE* debug_output_stream;

	jmp_buf exit_hook;
    int exit_hooked;
	int exit_allowed;
} rbase;

#define RLD_STREAM rbase.debug_output_stream


enum resource_validate_error_t {
	RVE_SUCCESS = 0,
	
	// blok zasob??w
	
	RVE_INVALID_MAGIC1,
	RVE_INVALID_MAGIC2,
	RVE_INVALID_CHECKSUM,
	
	// RT_MEMORY - blok pami??ci
	
	RVE_INVALID_HEAD_FENCE,
	RVE_INVALID_TAIL_FENCE
	
	// RT_STREAM
	// todo?
};

static struct limit_descriptor_t {
    enum heap_function_code_t call_type;

    // funkcje sterty
    struct {
        size_t singleshot;

        struct {
			size_t limit;
			size_t sum;
		} cumulative;
    } heap;

    // limit sukces??w
    struct {
        size_t limit;
        size_t counter;
    } success;

} rld_limit[__hfc_max];

static char* rld_severity_text[] = {
	[MSL_QUIET] = "<cisza>",
	[MSL_INFORMATION] = "Informacja",
	[MSL_WARNING] = "Ostrze??enie",
	[MSL_FAILURE] = BOLDRED("PORA??KA")
};

static char* rld_resource_validate_error_message[] = {
    [RVE_SUCCESS] = "Ok (RVE_SUCCESS)",
    [RVE_INVALID_MAGIC1] = "Zamazany pocz??tek bloku opisuj??cego zas??b (RVE_INVALID_MAGIC1)",
    [RVE_INVALID_MAGIC2] = "Zamazany koniec bloku opisuj??cego zas??b (RVE_INVALID_MAGIC2)",
    [RVE_INVALID_CHECKSUM] = "Zamazana suma kontrolna bloku opisuj??cego zas??b (RVE_INVALID_CHECKSUM)",
    [RVE_INVALID_HEAD_FENCE] = "Wykryto modyfikacj?? obszaru pami??ci przed zaalokowanym blokiem (RVE_INVALID_HEAD_FENCE)",
    [RVE_INVALID_TAIL_FENCE] = "Wykryto modyfikacj?? obszaru pami??ci za zaalokowanym blokiem (RVE_INVALID_TAIL_FENCE)"
};

static char* rld_message_text[] = {
	[RLD_UNDEFINED] = "B????d nieokre??lony; skontaktuj si?? z autorem",
	[RLD_HEAP_FUNCTIONS_DISABLED] = "Funkcje operacji na stercie s?? zablokowane; prosz?? ich nie u??ywa??",
	
	[RLD_MALLOC_NO_MEMORY] = "Brak wolnej pami??ci dla funkcji malloc",
	[RLD_MALLOC_NO_MEMORY_DUE_LIMIT] = "Brak wolnej pami??ci dla funkcji malloc (heap limit)",
	[RLD_MALLOC_NO_MEMORY_DUE_SINGLESHOT_LIMIT] = "Przekroczono limit alokowanego bloku w jednym wywo??aniu funkcji malloc()",
	[RLD_MALLOC_NO_MEMORY_DUE_CUMULATIVE_LIMIT] = "Przekroczono skumulowany limit alokowanej pami??ci dla funkcji malloc()",
	[RLD_MALLOC_FAILED_DUE_SUCCESS_LIMIT] = "Funkcja malloc() zako??czy??a si?? pora??k?? ze wgl??du na wyczerpany limit wywo??a??, kt??re mog?? zako??czy??  si?? sukcesem",
	[RLD_MALLOC_SUCCESSFUL] = "Blok pami??ci o ????danej wielko??ci zosta?? pomy??lnie przydzielony",

	[RLD_FREE_NULL] = "Pr??ba zwolnienia wska??nika NULL",
	[RLD_FREE_INVALID_POINTER] = "Pr??ba zwolnienia niezaalokowanego wcze??niej bloku pami??ci (nieznany wska??nik)",
	[RLD_FREE_SUCCESSFUL] = "Blok pami??ci zosta?? pomy??lnie zwolniony",
	
	[RLD_CALLOC_NO_MEMORY] = "Brak wolnej pami??ci dla funkcji calloc",
	[RLD_CALLOC_NO_MEMORY_DUE_LIMIT] = "Brak wolnej pami??ci dla funkcji calloc (heap limit)",
	[RLD_CALLOC_NO_MEMORY_DUE_SINGLESHOT_LIMIT] = "Przekroczono limit alokowanego bloku w jednym wywo??aniu funkcji calloc()",
	[RLD_CALLOC_NO_MEMORY_DUE_CUMULATIVE_LIMIT] = "Przekroczono skumulowany limit alokowanej pami??ci dla funkcji calloc()",
	[RLD_CALLOC_FAILED_DUE_SUCCESS_LIMIT] = "Funkcja calloc() zako??czy??a si?? pora??k?? ze wgl??du na wyczerpany limit wywo??a??, kt??re mog?? zako??czy??  si?? sukcesem",
	[RLD_CALLOC_SUCCESSFUL] = "Blok pami??ci o ????danej wielko??ci zosta?? pomy??lnie przydzielony",

	[RLD_REALLOC_NO_MEMORY] = "Brak wolnej pami??ci dla funkcji realloc; wielko???? bloku nie uleg??a zmianie",
	[RLD_REALLOC_NO_MEMORY_DUE_LIMIT] = "Brak wolnej pami??ci dla funkcji realloc; wielko???? bloku nie uleg??a zmianie (heap limit)",
	[RLD_REALLOC_NO_MEMORY_DUE_SINGLESHOT_LIMIT] = "Przekroczono limit alokowanego bloku w jednym wywo??aniu funkcji realloc()",
	[RLD_REALLOC_NO_MEMORY_DUE_CUMULATIVE_LIMIT] = "Przekroczono skumulowany limit alokowanej pami??ci dla funkcji realloc()",
	[RLD_REALLOC_FAILED_DUE_SUCCESS_LIMIT] = "Funkcja realloc() zako??czy??a si?? pora??k?? ze wgl??du na wyczerpany limit wywo??a??, kt??re mog?? zako??czy??  si?? sukcesem",
    [RLD_REALLOC_INVALID_POINTER] = "Pr??ba zmiany rozmiaru niezaalokowanego wcze??niej bloku pami??ci (nieznany wska??nik)",
	[RLD_REALLOC_SUCCESSFUL] = "Rozmiar bloku pami??ci zosta?? zmieniony pomy??lnie",

	//
	
	[RLD_STRDUP_NULL] = "Pr??ba duplikowania tekstu o wska??niku NULL",
	[RLD_STRDUP_NO_MEMORY] = "Brak wolnej pami??ci dla funkcji strdup",
	[RLD_STRDUP_NO_MEMORY_DUE_LIMIT] = "Brak wolnej pami??ci dla funkcji strdup (heap limit)",
	[RLD_STRDUP_NO_MEMORY_DUE_SINGLESHOT_LIMIT] = "Przekroczono limit alokowanego bloku w jednym wywo??aniu funkcji strdup()",
	[RLD_STRDUP_NO_MEMORY_DUE_CUMULATIVE_LIMIT] = "Przekroczono skumulowany limit alokowanej pami??ci dla funkcji strdup()",
	[RLD_STRDUP_FAILED_DUE_SUCCESS_LIMIT] = "Funkcja strdup() zako??czy??a si?? pora??k?? ze wgl??du na wyczerpany limit wywo??a??, kt??re mog?? zako??czy??  si?? sukcesem",
	[RLD_STRDUP_SUCCESSFUL] = "Pami???? dla kopii tekstu zosta??a pomy??lnie przydzielona",
	
	[RLD_STRNDUP_NULL] = "Pr??ba duplikowania tekstu o wska??niku NULL",
	[RLD_STRNDUP_NO_MEMORY] = "Brak wolnej pami??ci dla funkcji strndup",
	[RLD_STRNDUP_NO_MEMORY_DUE_LIMIT] = "Brak wolnej pami??ci dla funkcji strndup (heap limit)",
	[RLD_STRNDUP_NO_MEMORY_DUE_SINGLESHOT_LIMIT] = "Przekroczono limit alokowanego bloku w jednym wywo??aniu funkcji strndup()",
	[RLD_STRNDUP_NO_MEMORY_DUE_CUMULATIVE_LIMIT] = "Przekroczono skumulowany limit alokowanej pami??ci dla funkcji strndup()",
	[RLD_STRNDUP_FAILED_DUE_SUCCESS_LIMIT] = "Funkcja strndup() zako??czy??a si?? pora??k?? ze wgl??du na wyczerpany limit wywo??a??, kt??re mog?? zako??czy??  si?? sukcesem",
	[RLD_STRNDUP_SUCCESSFUL] = "Pami???? dla ograniczonej kopii tekstu zosta??a pomy??lnie przydzielona",


	[RLD_HEAP_BROKEN] = "Wykryto uszkodzenie pami??ci starty. Kt??ra?? z poprzednich operacji wysz??a po za sw??j zakres pami??ci.",
	[RLD_HEAP_DATA_OUT_OF_BOUNDS] = "Wykryto naruszenie granic bloku pami??ci.",


	//
	
	
	[RLD_FOPEN_SUCCESSFUL] = "Plik zosta?? pomy??lnie otwarty",
	[RLD_FOPEN_FAILED] = "Nie uda??o si?? otworzy?? pliku",
	[RLD_FOPEN_FAILED_DUE_SUCCESS_LIMIT] = "Funkcja fopen() zako??czy??a si?? pora??k?? ze wgl??du na wyczerpany limit wywo??a??, kt??re mog?? zako??czy??  si?? sukcesem",
	
	[RLD_FCLOSE_NULL_STREAM] = "Pr??ba zamkni??cia pliku reprezentowanego warto??ci?? NULL",
	[RLD_FCLOSE_INVALID_STREAM] = "Pr??ba zamkni??cia nieotwartego wcze??niej pliku (nieznany uchwyt pliku)",
	[RLD_FCLOSE_SUCCESSFUL] = "Plik zosta?? pomy??lnie zamkni??ty",
	
};


//
//
//
//
//

static const char* only_name(const char* full_path);
static uint32_t calc_checksum(const void* restrict buffer, size_t size);
static void update_checksum(struct resource_t* pres);
static enum resource_validate_error_t validate_resource(const struct resource_t* pres);
static int validate_heap(const char* caller_source_file, int caller_source_line);

static void rldebug_init(void)
{
	memset(&rbase, 0, sizeof(struct resource_base_t));
	srand(time(NULL));
	
	rbase.settings.lowest_reported_severity = MSL_INFORMATION;
	rbase.exit_allowed = 0;

	rbase.debug_output_stream = stdout;
//	rbase.debug_output_stream = stderr;
}


static struct resource_t* create_resource(enum resource_type_t res_type, const char* source_file, int source_line)
{
	struct resource_t* pres = (struct resource_t*)malloc(sizeof(struct resource_t));
	memset(pres, 0, sizeof(struct resource_t));
	assert(pres != NULL);

	pres->type = res_type;
	pres->pnext = NULL;
	pres->pprev = NULL;

	pres->source_file = source_file;
	pres->source_line = source_line;
	
	pres->magic1 = RLD_MAGIC1;
	pres->magic2 = RLD_MAGIC2;
	
	if (res_type == RT_MEMORY)
	{
		//pres->memory.head_pattern = ((uint64_t)rand() << 32) | (uint64_t)rand();
		//pres->memory.tail_pattern = ((uint64_t)rand() << 32) | (uint64_t)rand();
		
		for (int i = 0; i < 32; i++)
		{
			pres->memory.head_fence.pattern[i] = i + 1;
			pres->memory.tail_fence.pattern[i] = 32 - i;
		}
	}

	return pres;	
}

static void remove_resource(struct resource_t** ppres)
{
    assert(ppres != NULL && "remove_resource: pprese == NULL");
	struct resource_t* pres = *ppres;
	
	if (pres->type == RT_STREAM)
	{
		// nic do roboty
	} else if (pres->type == RT_MEMORY)
	{
		// nic do roboty
	}
	
	if (pres->pnext == NULL && pres->pprev == NULL && rbase.phead == pres && rbase.ptail == pres)
	{
		// tylko jeden element
		rbase.phead = NULL;
		rbase.ptail = NULL;
	} else if (pres->pprev == NULL && rbase.phead == pres)
	{
		// pierwszy
		rbase.phead = rbase.phead->pnext;
		rbase.phead->pprev = NULL;
		update_checksum(rbase.phead);
	} else if (pres->pnext == NULL && rbase.ptail == pres)
	{
		// ostatni
		rbase.ptail = rbase.ptail->pprev;
		rbase.ptail->pnext = NULL;
		update_checksum(rbase.ptail);
	} else if (pres->pnext != NULL && pres->pprev != NULL)
	{
		// ??rodeczek
		struct resource_t *p1, *p2;
		p1 = pres->pprev;
		p2 = pres->pnext;
		pres->pprev->pnext = pres->pnext;
		pres->pnext->pprev = pres->pprev;
		update_checksum(p1);
		update_checksum(p2);
	} else
		assert(0 && "Naruszona sp??jno???? sterty");

	free(pres);
	*ppres = NULL;
}

static void add_resource(struct resource_t *pres)
{
    assert(pres != NULL && "add_resource: pres == NULL");

	if (rbase.phead == NULL)
	{
		rbase.phead = pres;
		rbase.ptail = pres;
	} else
	{
		pres->pprev = rbase.ptail;
		rbase.ptail->pnext = pres;
		rbase.ptail = pres;
	}
	
	update_checksum(pres);
	if (pres->pprev)
		update_checksum(pres->pprev);
		

	// Je??li dodawany zas??b jest blokiem pami??ci, to zwi??ksz globalne statystyki zaj??cia sterty
	if (pres->type == RT_MEMORY)
	{
		rbase.current_heap_size += pres->memory.size;
		rbase.top_heap_size = MAX(rbase.current_heap_size, rbase.top_heap_size);
	}
	//pres->checksum = 0;
	//pres->checksum = calc_checksum(pres, sizeof(struct resource_t));
}


static struct resource_t* find_resource(enum resource_type_t res_type, const void* handle)
{
	struct resource_t *presource = rbase.phead;
	for( ;presource != NULL; presource = presource->pnext)
	{
		if (presource->type != res_type)
			continue;
		
		if (res_type == RT_MEMORY && presource->memory.base_pointer == handle)
			return presource;
		if (res_type == RT_STREAM && presource->stream.stream == handle)
			return presource;
	}
	
	return NULL;
}

char* print_source_location(char* buffer, size_t buffer_size, const char* source_name, int source_line)
{
    if (source_name == NULL || source_line == -1)
    {
        if (buffer_size)
            buffer[0] = '\x0';
        return buffer;
    }

#if defined(_HTML_OUTPUT)
    snprintf(buffer, buffer_size, "<a href=\"source/%s.html#line-%d\">%s:%d</a>",
        only_name(source_name), source_line, only_name(source_name), source_line);
#else
    snprintf(buffer, buffer_size, "%s:%d", only_name(source_name), source_line);
#endif
    return buffer;
}

static void report(enum message_severity_level_t severity, enum rld_message_t msg_id, const char* source_name, int source_line, const char* message)
{
	if (severity >= rbase.settings.lowest_reported_severity)
	{

        char location[128];
        print_source_location(location, sizeof(location), source_name, source_line);

		if (message == NULL)
		    if (source_name == NULL || source_line == -1)
	    		fprintf(RLD_STREAM, BOLDPINK("Analiza zasob??w")": %s: " BOLD("%s") "\n",
		    		rld_severity_text[severity], rld_message_text[msg_id]);
			else
				fprintf(RLD_STREAM, BOLDPINK("Analiza zasob??w")": %s dla %s: " BOLD("%s") "\n",
    				rld_severity_text[severity], location, rld_message_text[msg_id]);

		else
		    if (source_name == NULL || source_line == -1)
                fprintf(RLD_STREAM, BOLDPINK("Analiza zasob??w")": %s: " BOLD("%s") " [%s]\n",
                    rld_severity_text[severity], rld_message_text[msg_id], message);
            else
                fprintf(RLD_STREAM, BOLDPINK("Analiza zasob??w")": %s dla %s: " BOLD("%s") " [%s]\n",
    				rld_severity_text[severity], location, rld_message_text[msg_id], message);
	}

	fflush(RLD_STREAM);
	
	if (severity == MSL_FAILURE)
		raise(SIGHEAP);
}

static void* setup_base_pointer(struct resource_t* pres)
{
	assert(pres->type == RT_MEMORY);
	
	uint8_t* base = pres->memory.base_pointer;
	*(struct block_fence_t*)base = pres->memory.head_fence;
	*(struct block_fence_t*)(base + sizeof(struct block_fence_t) + pres->memory.size) = pres->memory.tail_fence;
	
	void* user_space_pointer = base + sizeof(struct block_fence_t);
	return user_space_pointer;
}

static void* get_base_pointer(void* user_space_pointer)
{
	if (user_space_pointer == NULL)
		return NULL;
	return (uint8_t*)user_space_pointer - sizeof(struct block_fence_t);
}

void* _rldebug_heap_wrapper(enum heap_function_code_t call_type, void* user_pointer, size_t number, size_t size, const char* source_name, int source_line)
{
    char msg[128];
	validate_heap(source_name, source_line);

    //
    //
    //
	if (call_type == HFC_MALLOC)
	{
		// przekazanie liczby ujemnej daje liczb?? dodatni?? ponad SIZE_MAX/2
		if (number > SIZE_MAX >> 1)
			return NULL;
		
        if (rbase.settings.heap.global_disable)
			report(MSL_FAILURE, RLD_HEAP_FUNCTIONS_DISABLED, source_name, source_line, NULL);

        // limit wywo??a?? mog??cych zako??czy?? si?? sukcesem
		if (++rld_limit[call_type].success.counter > rld_limit[call_type].success.limit)
		{
			report(MSL_INFORMATION, RLD_MALLOC_FAILED_DUE_SUCCESS_LIMIT, source_name, source_line, NULL);
			return NULL;
		}

        // limit pojedynczej alokacji
		if (number > rld_limit[call_type].heap.singleshot)
		{
		    snprintf(msg, sizeof(msg), "wynosi on %lu bajt??w", rld_limit[call_type].heap.singleshot);
		    report(MSL_INFORMATION, RLD_MALLOC_NO_MEMORY_DUE_SINGLESHOT_LIMIT, source_name, source_line, msg);
			return NULL;
		}

        // skumulowany limit alokacji
		if (number + rld_limit[call_type].heap.cumulative.sum > rld_limit[call_type].heap.cumulative.limit)
		{
		    snprintf(msg, sizeof(msg), "wynosi on %lu bajt??w", rld_limit[call_type].heap.cumulative.limit);
		    report(MSL_INFORMATION, RLD_MALLOC_NO_MEMORY_DUE_CUMULATIVE_LIMIT, source_name, source_line, msg);
			return NULL;
		}

		// ogranicznik sterty
		if (rbase.settings.heap.global_limit_active)
			if (rbase.current_heap_size + number > rbase.settings.heap.global_limit_value)
			{
				report(MSL_INFORMATION, RLD_MALLOC_NO_MEMORY_DUE_LIMIT, source_name, source_line, NULL);
				return NULL;
			}

		// normalna alokacja
		uint8_t* new_base_pointer = (uint8_t*)malloc(number + sizeof(struct block_fence_t) * 2);
		if (new_base_pointer == NULL)
		{
			report(MSL_INFORMATION, RLD_MALLOC_NO_MEMORY, source_name, source_line, NULL);
			return NULL;
		}

		struct resource_t *pres = create_resource(RT_MEMORY, source_name, source_line);
		pres->memory.size = number;
		pres->memory.base_pointer = new_base_pointer;
		pres->memory.allocated_by = call_type;

		add_resource(pres);
		rld_limit[call_type].heap.cumulative.sum += pres->memory.size;
		report(MSL_INFORMATION, RLD_MALLOC_SUCCESSFUL, source_name, source_line, NULL);

		return setup_base_pointer(pres);
	}

    //
    //
    //
	if (call_type == HFC_FREE)
	{
        if (rbase.settings.heap.global_disable)
			report(MSL_FAILURE, RLD_HEAP_FUNCTIONS_DISABLED, source_name, source_line, NULL);

		// libc - brak akcji
		if (user_pointer == NULL)
		{
			report(MSL_WARNING, RLD_FREE_NULL, source_name, source_line, NULL);
			return NULL;
		}

		void* base_pointer = get_base_pointer(user_pointer);

		struct resource_t *pres = find_resource(RT_MEMORY, base_pointer);
		if (pres == NULL)
			report(MSL_FAILURE, RLD_FREE_INVALID_POINTER, source_name, source_line, NULL);

        assert(IS_ALLOCATION_FUNCTION_CODE(pres->memory.allocated_by));
		rbase.current_heap_size -= pres->memory.size;
		rld_limit[pres->memory.allocated_by].heap.cumulative.sum -= pres->memory.size;
		assert(rld_limit[pres->memory.allocated_by].heap.cumulative.sum <= (SIZE_MAX >> 1));

		remove_resource(&pres);
		free(base_pointer);
		report(MSL_INFORMATION, RLD_FREE_SUCCESSFUL, source_name, source_line, NULL);

		return NULL;
	}
	
    //
    //
    //
	if (call_type == HFC_CALLOC)
	{
		// przekazanie liczby ujemnej daje liczb?? dodatni?? ponad SIZE_MAX/2
		size_t bytes = number * size;
		if (bytes > SIZE_MAX >> 1)
			return NULL;
		
        if (rbase.settings.heap.global_disable)
			report(MSL_FAILURE, RLD_HEAP_FUNCTIONS_DISABLED, source_name, source_line, NULL);

        // limit wywo??a?? mog??cych zako??czy?? si?? sukcesem
		if (++rld_limit[call_type].success.counter > rld_limit[call_type].success.limit)
		{
			report(MSL_INFORMATION, RLD_CALLOC_FAILED_DUE_SUCCESS_LIMIT, source_name, source_line, NULL);
			return NULL;
		}


        // limit pojedynczej alokacji
		if (bytes > rld_limit[call_type].heap.singleshot)
		{
		    snprintf(msg, sizeof(msg), "wynosi on %lu bajt??w", rld_limit[call_type].heap.singleshot);
		    report(MSL_INFORMATION, RLD_CALLOC_NO_MEMORY_DUE_SINGLESHOT_LIMIT, source_name, source_line, msg);
			return NULL;
		}

        // skumulowany limit alokacji
		if (number + rld_limit[call_type].heap.cumulative.sum > rld_limit[call_type].heap.cumulative.limit)
		{
		    snprintf(msg, sizeof(msg), "wynosi on %lu bajt??w", rld_limit[call_type].heap.cumulative.limit);
		    report(MSL_INFORMATION, RLD_CALLOC_NO_MEMORY_DUE_CUMULATIVE_LIMIT, source_name, source_line, msg);
			return NULL;
		}

		// ogranicznik sterty
		if (rbase.settings.heap.global_limit_active)
			if (rbase.current_heap_size + bytes > rbase.settings.heap.global_limit_value)
			{
				report(MSL_INFORMATION, RLD_CALLOC_NO_MEMORY_DUE_LIMIT, source_name, source_line, NULL);
				return NULL;
			}
			
		// normalna alokacja
		uint8_t* new_base_pointer = (uint8_t*)malloc(bytes + sizeof(struct block_fence_t) * 2);
		if (new_base_pointer == NULL)
		{
			report(MSL_INFORMATION, RLD_CALLOC_NO_MEMORY, source_name, source_line, NULL);
			return NULL;
		} else
			memset(new_base_pointer, 0x00, bytes + sizeof(struct block_fence_t) * 2);

		
		struct resource_t *pres = create_resource(RT_MEMORY, source_name, source_line);
		pres->memory.size = bytes;
		pres->memory.base_pointer = new_base_pointer;
		pres->memory.allocated_by = call_type;

		add_resource(pres);
		rld_limit[call_type].heap.cumulative.sum += pres->memory.size;
		report(MSL_INFORMATION, RLD_CALLOC_SUCCESSFUL, source_name, source_line, NULL);

		return setup_base_pointer(pres);
	}
	
    //
    // void* realloc(void*, size_t)
    //
	if (call_type == HFC_REALLOC)
	{
		// przekazanie liczby ujemnej daje liczb?? dodatni?? ponad SIZE_MAX/2
		if (number > SIZE_MAX >> 1)
			return NULL;
		
        if (rbase.settings.heap.global_disable)
			report(MSL_FAILURE, RLD_HEAP_FUNCTIONS_DISABLED, source_name, source_line, NULL);

        // limit wywo??a?? mog??cych zako??czy?? si?? sukcesem
		if (++rld_limit[call_type].success.counter > rld_limit[call_type].success.limit)
		{
			report(MSL_INFORMATION, RLD_REALLOC_FAILED_DUE_SUCCESS_LIMIT, source_name, source_line, NULL);
			return NULL;
		}

		// normalna alokacja		
		void* base_pointer = get_base_pointer(user_pointer);
		struct resource_t *pres = find_resource(RT_MEMORY, base_pointer);
		if (base_pointer != NULL && pres == NULL)
			report(MSL_FAILURE, RLD_REALLOC_INVALID_POINTER, source_name, source_line, NULL);

		// Je??eli wywo??anie zmienia rozmiar bufora, to analizuj tylko przyrost
		int64_t allocation_delta = (int64_t)number;
		if (pres != NULL)
			allocation_delta = (int64_t)number - (int64_t)pres->memory.size;

		// nie ma ??adnej zmiany?
		if (allocation_delta == 0)
			return user_pointer;
			
		// limit pojedynczej alokacji
		if ((pres == NULL) && (number > rld_limit[call_type].heap.singleshot) ||
			(pres != NULL) && (number > pres->memory.size)
						   && (allocation_delta > (int64_t)rld_limit[call_type].heap.singleshot))
		{
		    snprintf(msg, sizeof(msg), "wynosi on %lu bajt??w", rld_limit[call_type].heap.singleshot);
		    report(MSL_INFORMATION, RLD_REALLOC_NO_MEMORY_DUE_SINGLESHOT_LIMIT, source_name, source_line, msg);
			return NULL;
		}

        // skumulowany limit alokacji
		if ((pres == NULL && (number + rld_limit[call_type].heap.cumulative.sum > rld_limit[call_type].heap.cumulative.limit)) ||
			(pres != NULL && (number > pres->memory.size)
						  && (allocation_delta + (int64_t)rld_limit[call_type].heap.cumulative.sum > (int64_t)rld_limit[call_type].heap.cumulative.limit)))
			{
				snprintf(msg, sizeof(msg), "wynosi on %lu bajt??w", rld_limit[call_type].heap.cumulative.limit);
				report(MSL_INFORMATION, RLD_REALLOC_NO_MEMORY_DUE_CUMULATIVE_LIMIT, source_name, source_line, msg);
				return NULL;
			}

		// ogranicznik sterty
		if (rbase.settings.heap.global_limit_active)
			if ((int64_t)rbase.current_heap_size + allocation_delta > (int64_t)rbase.settings.heap.global_limit_value)
			{
				report(MSL_INFORMATION, RLD_REALLOC_NO_MEMORY_DUE_LIMIT, source_name, source_line, NULL);
				return NULL;
			}
				
		void* new_base_pointer = realloc(base_pointer, number + sizeof(struct block_fence_t) * 2);
		if (new_base_pointer == NULL)
		{
			// Teraz to ju?? naprawd?? brakuje pami??ci :)
			report(MSL_INFORMATION, RLD_REALLOC_NO_MEMORY, source_name, source_line, NULL);
			return NULL;
		}

		if (base_pointer == NULL)
		{
			//
			// Nie przekazano wska??nika do realloc() - funkcja dzia??a jak malloc 
			//
			pres = create_resource(RT_MEMORY, source_name, source_line);
			pres->memory.size = number;
			pres->memory.base_pointer = new_base_pointer;
			pres->memory.allocated_by = call_type;

			add_resource(pres);
			rld_limit[call_type].heap.cumulative.sum += pres->memory.size;
			report(MSL_INFORMATION, RLD_REALLOC_SUCCESSFUL, source_name, source_line, NULL);

			return setup_base_pointer(pres);
		} else
		{

			size_t old_size = pres->memory.size;
			enum heap_function_code_t old_code = pres->memory.allocated_by;
            assert(IS_ALLOCATION_FUNCTION_CODE(old_code));

			pres->memory.allocated_by = call_type;
			pres->memory.size = number;
			pres->memory.base_pointer = new_base_pointer;
			pres->source_line = source_line;
			pres->source_file = source_name;
			
			update_checksum(pres);
			if (pres->pprev)
				update_checksum(pres->pprev);
			if (pres->pnext)
				update_checksum(pres->pnext);

			// wycofaj liczb?? bajt??w zaalokowan?? w tym bloku poprzednim wywo??aniem (old_code)
			rbase.current_heap_size -= old_size;
			rld_limit[old_code].heap.cumulative.sum -= old_size;
    		assert(rld_limit[old_code].heap.cumulative.sum < (SIZE_MAX >> 1));

			// uzupe??nij liczniki o now?? wielko???? bloku
			rbase.current_heap_size += pres->memory.size;
			rld_limit[call_type].heap.cumulative.sum += pres->memory.size;

			rbase.top_heap_size = MAX(rbase.current_heap_size, rbase.top_heap_size);
		}

		report(MSL_INFORMATION, RLD_REALLOC_SUCCESSFUL, source_name, source_line, NULL);
		return setup_base_pointer(pres);
	}	
	
	assert(0 && "_rldebug_heap_wrapper: niezaimplementowana funkcja");
}

char* _rldebug_str_wrapper(enum heap_function_code_t call_type, const char* pstring, size_t number, const char* source_file, int source_line)
{
    char msg[128];
	validate_heap(source_file, source_line);

    //
    //
    //
	if (call_type == HFC_STRDUP)
	{
        if (rbase.settings.heap.global_disable)
			report(MSL_FAILURE, RLD_HEAP_FUNCTIONS_DISABLED, source_file, source_line, NULL);

		if (pstring == NULL)
			report(MSL_FAILURE, RLD_STRDUP_NULL, source_file, source_line, NULL);

        // limit wywo??a?? mog??cych zako??czy?? si?? sukcesem
		if (++rld_limit[call_type].success.counter > rld_limit[call_type].success.limit)
		{
			report(MSL_INFORMATION, RLD_STRDUP_FAILED_DUE_SUCCESS_LIMIT, source_file, source_line, NULL);
			return NULL;
		}

		size_t bytes = strlen(pstring) + 1;

        // limit pojedynczej alokacji
		if (bytes > rld_limit[call_type].heap.singleshot)
		{
		    snprintf(msg, sizeof(msg), "wynosi on %lu bajt??w", rld_limit[call_type].heap.singleshot);
		    report(MSL_INFORMATION, RLD_STRDUP_NO_MEMORY_DUE_SINGLESHOT_LIMIT, source_file, source_line, msg);
			return NULL;
		}

        // skumulowany limit alokacji
		if (number + rld_limit[call_type].heap.cumulative.sum > rld_limit[call_type].heap.cumulative.limit)
		{
		    snprintf(msg, sizeof(msg), "wynosi on %lu bajt??w", rld_limit[call_type].heap.cumulative.limit);
		    report(MSL_INFORMATION, RLD_STRDUP_NO_MEMORY_DUE_CUMULATIVE_LIMIT, source_file, source_line, msg);
			return NULL;
		}

		// ogranicznik sterty
		if (rbase.settings.heap.global_limit_active)
			if (rbase.current_heap_size + bytes > rbase.settings.heap.global_limit_value)
			{
				report(MSL_INFORMATION, RLD_STRDUP_NO_MEMORY_DUE_LIMIT, source_file, source_line, NULL);
				return NULL;
			}

		// normalna alokacja		
		uint8_t* new_base_pointer = (uint8_t*)malloc(bytes + sizeof(struct block_fence_t) * 2);
		if (new_base_pointer == NULL)
		{
			report(MSL_INFORMATION, RLD_STRDUP_NO_MEMORY, source_file, source_line, NULL);
			return NULL;
		}

		struct resource_t *pres = create_resource(RT_MEMORY, source_file, source_line);
		pres->memory.size = bytes;
		pres->memory.base_pointer = new_base_pointer;
		pres->memory.allocated_by = call_type;

		add_resource(pres);
		rld_limit[call_type].heap.cumulative.sum += pres->memory.size;
		report(MSL_INFORMATION, RLD_STRDUP_SUCCESSFUL, source_file, source_line, NULL);

		char* user_pointer = setup_base_pointer(pres);
		strcpy(user_pointer, pstring);

		return user_pointer;
	}
	
    //
    //
    //
	if (call_type == HFC_STRNDUP)
	{
        if (rbase.settings.heap.global_disable)
			report(MSL_FAILURE, RLD_HEAP_FUNCTIONS_DISABLED, source_file, source_line, NULL);

		if (pstring == NULL)
			report(MSL_FAILURE, RLD_STRNDUP_NULL, source_file, source_line, NULL);

        // limit wywo??a?? mog??cych zako??czy?? si?? sukcesem
		if (++rld_limit[call_type].success.counter > rld_limit[call_type].success.limit)
		{
			report(MSL_INFORMATION, RLD_STRNDUP_FAILED_DUE_SUCCESS_LIMIT, source_file, source_line, NULL);
			return NULL;
		}

		size_t bytes = strlen(pstring);
		bytes = MIN(bytes, number);

        // limit pojedynczej alokacji
		if (bytes > rld_limit[call_type].heap.singleshot)
		{
		    char msg[128];
		    snprintf(msg, sizeof(msg), "wynosi on %lu bajt??w", rld_limit[call_type].heap.singleshot);
		    report(MSL_INFORMATION, RLD_STRNDUP_NO_MEMORY_DUE_SINGLESHOT_LIMIT, source_file, source_line, msg);
			return NULL;
		}

        // skumulowany limit alokacji
		if (number + rld_limit[call_type].heap.cumulative.sum > rld_limit[call_type].heap.cumulative.limit)
		{
		    snprintf(msg, sizeof(msg), "wynosi on %lu bajt??w", rld_limit[call_type].heap.cumulative.limit);
		    report(MSL_INFORMATION, RLD_STRNDUP_NO_MEMORY_DUE_CUMULATIVE_LIMIT, source_file, source_line, msg);
			return NULL;
		}

		// ogranicznik sterty
		if (rbase.settings.heap.global_limit_active)
			if (rbase.current_heap_size + bytes > rbase.settings.heap.global_limit_value)
			{
				report(MSL_INFORMATION, RLD_STRNDUP_NO_MEMORY_DUE_LIMIT, source_file, source_line, NULL);
				return NULL;
			}

		// normalna alokacja		
		uint8_t* new_base_pointer = (uint8_t*)malloc(bytes + 1 + sizeof(struct block_fence_t) * 2);
		if (new_base_pointer == NULL)
		{
			report(MSL_INFORMATION, RLD_STRNDUP_NO_MEMORY, source_file, source_line, NULL);
			return NULL;
		}

		struct resource_t *pres = create_resource(RT_MEMORY, source_file, source_line);
		pres->memory.size = bytes + 1;
		pres->memory.base_pointer = new_base_pointer;
		pres->memory.allocated_by = call_type;

		add_resource(pres);
		rld_limit[call_type].heap.cumulative.sum += pres->memory.size;
		report(MSL_INFORMATION, RLD_STRNDUP_SUCCESSFUL, source_file, source_line, NULL);

		char* user_pointer = setup_base_pointer(pres);
		memcpy(user_pointer, pstring, bytes);
		user_pointer[bytes] = '\x0';

		return user_pointer;			
	}

	assert(0 && "_rldebug_str_wrapper: niezaimplementowana funkcja");
}


void* _rldebug_io_wrapper(enum heap_function_code_t call_type, FILE* stream, const char* stream_name, const char* stream_mode, const char* source_file, int source_line)
{
    char sys_message[256];
	validate_heap(source_file, source_line);

    //
    //
    //
	if (call_type == HFC_FOPEN)
	{
        // limit wywo??a?? mog??cych zako??czy?? si?? sukcesem
		if (++rld_limit[call_type].success.counter > rld_limit[call_type].success.limit)
		{
			report(MSL_INFORMATION, RLD_FOPEN_FAILED_DUE_SUCCESS_LIMIT, source_file, source_line, NULL);
			return NULL;
		}

		FILE* fhandle = fopen(stream_name, stream_mode);
		if (fhandle == NULL)
		{
			snprintf(sys_message, sizeof(sys_message), "%s; errno=%d", strerror(errno), errno);
			report(MSL_INFORMATION, RLD_FOPEN_FAILED, source_file, source_line, sys_message);
			return NULL;
		}

		struct resource_t *pres = create_resource(RT_STREAM, source_file, source_line);
		pres->stream.name = strdup(stream_name);
		pres->stream.mode = strdup(stream_mode);
		pres->stream.stream = fhandle;

		assert(pres->stream.name != NULL && pres->stream.mode != NULL);

		add_resource(pres);
		report(MSL_INFORMATION, RLD_FOPEN_SUCCESSFUL, source_file, source_line, NULL);

		return fhandle;
	}
	
    //
    //
    //
	if (call_type == HFC_FCLOSE)
	{
		if (stream == NULL)
			report(MSL_FAILURE, RLD_FCLOSE_NULL_STREAM, source_file, source_line, NULL);

		struct resource_t *pres = find_resource(RT_STREAM, (void*)stream);

		if (pres == NULL)
			report(MSL_FAILURE, RLD_FCLOSE_INVALID_STREAM, source_file, source_line, NULL);

		intptr_t result = fclose(stream);
		remove_resource(&pres);
		report(MSL_INFORMATION, RLD_FCLOSE_SUCCESSFUL, source_file, source_line, NULL);
		return (void*)result;
	}

	assert(0 && "_rldebug_mem_wrapper: niezaimplementowana funkcja");
}

void __attribute__((noreturn)) _rldebug_stdlib_noreturn_wrapper(enum heap_function_code_t call_type, int iarg, const char* source_file, int source_line)
{
	validate_heap(source_file, source_line);

    //
    //
    //
    if (call_type == HFC_EXIT)
    {
		if (rbase.exit_hooked)
			longjmp(rbase.exit_hook, 0x0100 | (iarg) & 0xFF);

		if (rbase.exit_allowed)
			exit(iarg);

		// nie u??ywa?? exit()
        char location[128];
        print_source_location(location, sizeof(location), source_file, source_line);
		fprintf(RLD_STREAM, "\n" BOLDRED("*** U??yto funkcji exit(int) w %s. Pozwala on na natychmiastowe wyj??cie z programu, co uniemo??liwia finalizacj?? test??w.\n"), location);
		fprintf(RLD_STREAM, "*** Prosz?? poprawi?? sw??j kod tak aby niewykorzystywa?? funkcji exit()\n");
		fprintf(RLD_STREAM, "*** W przypadku w??tpliwo??ci prosz?? skontaktowa?? si?? z autorem testu.\n");
        raise(SIGTERM);
    }

	assert(0 && "_rldebug_stdlib_noreturn_wrapper: niezaimplementowany call_type");
}


int rldebug_show_leaked_resources(int force_empty_summary)
{
	uint32_t blocks = 0;
	uint32_t streams = 0;
	uint64_t memory_leaked = 0;
	
	// policz elementy
	for(struct resource_t *presource = rbase.phead; presource != NULL; presource = presource->pnext)
	{
		blocks += presource->type == RT_MEMORY;
		streams += presource->type == RT_STREAM;
	}

	if (blocks || streams || force_empty_summary)
	{
		//fprintf(RLD_STREAM, BYELLOW("** "BOLD("RLDebug")" :: Analizator wycieku zasob??w ***")"\n");
		fflush(RLD_STREAM);
	}

	// pami???? - lista wyciek??w
	if (blocks > 0)
	{
		fprintf(RLD_STREAM, "\n" BOLDRED("Wycieki pami??ci") ":\n");
		fprintf(RLD_STREAM, "--------------------------------------------\n");
		fprintf(RLD_STREAM, " ID                Adres       Plik ??r??d??owy\n");
		fprintf(RLD_STREAM, "           Liczba bajt??w       Numer linii  \n");
		fprintf(RLD_STREAM, "--------------------------------------------\n");
		fflush(RLD_STREAM);
		
		int i = 1; 
		for(struct resource_t *presource = rbase.phead; presource != NULL; presource = presource->pnext, i++)
		{
			if (presource->type != RT_MEMORY)
				continue;

#if defined(_HTML_OUTPUT)
			fprintf(RLD_STREAM, " %-3d  %18p       <a href=\"source/%s.html#line-%d\">%s</a>\n", i, presource->memory.base_pointer,
			    only_name(presource->source_file), presource->source_line, only_name(presource->source_file));
#else
			fprintf(RLD_STREAM, " %-3d  %18p       %s\n", i, presource->memory.base_pointer, only_name(presource->source_file));
#endif
			fprintf(RLD_STREAM, "      %18lu       %d\n",    presource->memory.size, presource->source_line);
			fflush(RLD_STREAM);
			memory_leaked += presource->memory.size;
		}
		
		fprintf(RLD_STREAM, "--------------------------------------------\n");
	}
	
	// pami???? - podsumowanie wyciek??w
	if (blocks > 0 || force_empty_summary)
	{
	    if (blocks > 0)
	    {
            fprintf(RLD_STREAM, "Liczba niezwolnionych blok??w pami??ci: " BOLDRED("%d") " blok(??w)\n", blocks);
            fprintf(RLD_STREAM, "Sumaryczna wielko???? wycieku pami??ci: " BOLDRED("%lu") " bajt(??w)\n", memory_leaked);
        } else
            fprintf(RLD_STREAM, BOLDGREEN("Wszystkie bloki pami??ci zosta??y pomy??lnie zwolnione - brak wyciek??w.") "\n");
	}
	
	// pliki - lista niezamkni??tych plik??w
	if (streams > 0)
	{
//		if (blocks)
//			fprintf(RLD_STREAM, "\n");
		fprintf(RLD_STREAM, "\n"BOLDRED("Niezamkni??te pliki")":\n");
		fprintf(RLD_STREAM, "--------------------------------------------\n");
		fprintf(RLD_STREAM, " ID  Nazwa                     Plik ??r??d??owy\n");
		fprintf(RLD_STREAM, "     Tryb                      Numer linii  \n");
		fprintf(RLD_STREAM, "--------------------------------------------\n");
		fflush(RLD_STREAM);
		
		int i = 1; 
		for(struct resource_t *presource = rbase.phead; presource != NULL; presource = presource->pnext, i++)
		{
			if (presource->type != RT_STREAM)
				continue;
				
			char fname[32] = {0};
			
			if (strlen(presource->stream.name) > 25)
			{
				strncpy(fname, presource->stream.name, 25-5);
				strcat(fname, "(...)");
			} else
				strncpy(fname, presource->stream.name, 25);
			

#if defined(_HTML_OUTPUT)
			fprintf(RLD_STREAM, " %-3d %-25s <a href=\"source/%s.html#line-%d\">%s</a>\n", i, fname,
			    only_name(presource->source_file), presource->source_line, only_name(presource->source_file));
#else
			fprintf(RLD_STREAM, " %-3d %-25s %s\n", i, fname, only_name(presource->source_file));
#endif

			fprintf(RLD_STREAM, "     %-25s %d\n",    presource->stream.mode, presource->source_line);
			fflush(RLD_STREAM);
		}
		
		fprintf(RLD_STREAM, "--------------------------------------------\n");

	}
	
	// pliki - podsumowanie
	if (streams > 0 || force_empty_summary)
	{
	    if (streams > 0)
		    fprintf(RLD_STREAM, "Liczba niezamkni??tych plik??w: "BOLDRED("%d")"\n", streams);
		else
		    fprintf(RLD_STREAM, BOLDGREEN("Wszystkie pliki zosta??y zamkni??te.\n"));
	}

	assert(memory_leaked == rbase.current_heap_size);

    if (force_empty_summary)
    {
        fprintf(RLD_STREAM, BOLDGREEN("Nie wykryto uszkodzenia sterty.\n"));
        fprintf(RLD_STREAM, "\n");
    }

	return streams + blocks;
}


size_t rldebug_heap_get_leak_size(void)
{
	size_t leak = 0;

	// policz elementy
	for(struct resource_t *presource = rbase.phead; presource != NULL; presource = presource->pnext)
	{
	    if (presource->type != RT_MEMORY)
	        continue;

	    leak += presource->memory.size;
	}

	return leak;
}

size_t rldebug_get_block_size(const void* ptr)
{
    for(struct resource_t *presource = rbase.phead; presource != NULL; presource = presource->pnext)
    {
        if (presource->type != RT_MEMORY)
            continue;
        if (ptr == (struct block_fence_t*)presource->memory.base_pointer + 1)
            return presource->memory.size;

    }
    return RLD_UNKNOWN_POINTER;
}

static const char* only_name(const char* full_path)
{
	char* p = strrchr(full_path, '/');
	return p ? p + 1 : full_path;
}

static uint32_t calc_checksum(const void* restrict buffer, size_t size)
{
	uint32_t chk = 0;
	const uint8_t* restrict ptr = (const uint8_t* restrict)buffer;
	while(size--)
		chk = ((chk ^ *ptr++) << 1) ^ !(chk & 1 << 31);
	return chk;
}

static void update_checksum(struct resource_t* pres)
{
	pres->checksum = 0;
	pres->checksum = calc_checksum(pres, sizeof(struct resource_t));
}


static enum resource_validate_error_t validate_resource(const struct resource_t* pres)
{
	// wst??pne sprawdzenie
	if (pres->magic1 != RLD_MAGIC1)
		return RVE_INVALID_MAGIC1; // uszkodzona magiczna liczba na pocz??tku deskryptora zasobu
	if (pres->magic2 != RLD_MAGIC2)
		return RVE_INVALID_MAGIC2; // uszkodzona magiczna liczba na ko??cu deskryptora zasobu
		
	// i test w??a??ciwy
	struct resource_t temp = *pres;
	temp.checksum = 0;
	uint32_t chk = calc_checksum(&temp, sizeof(struct resource_t));
	if (chk != pres->checksum)
		return RVE_INVALID_CHECKSUM; // suma kontrolna zasobu jest b????dna
		
	if (pres->type == RT_MEMORY)
	{
		struct block_fence_t* head_fence = (struct block_fence_t*)pres->memory.base_pointer;
		struct block_fence_t* tail_fence = (struct block_fence_t*)((uint8_t*)pres->memory.base_pointer + sizeof(struct block_fence_t) + pres->memory.size);
		if (memcmp(head_fence, &pres->memory.head_fence, sizeof(struct block_fence_t)) != 0)
			return RVE_INVALID_HEAD_FENCE; // uszkodzony p??otek na pocz??tku bloku danych
		if (memcmp(tail_fence, &pres->memory.tail_fence, sizeof(struct block_fence_t)) != 0)
			return RVE_INVALID_TAIL_FENCE; // uszkodzony p??otek na ko??cu bloku danych
	}
	
	return RVE_SUCCESS; // wszystko wydaje si?? by?? w porz??dku
}

static int validate_heap(const char* caller_source_file, int caller_source_line)
{
	for(struct resource_t *presource = rbase.phead; presource != NULL; presource = presource->pnext)
	{
		int vderror;
		if ((vderror = validate_resource(presource)) != RVE_SUCCESS)
		{
			char msg[512];
			//sprintf(msg, "vderror=%d", vderror);
			
			if (vderror == RVE_INVALID_MAGIC1 || vderror == RVE_INVALID_MAGIC2 || vderror == RVE_INVALID_CHECKSUM)
			{
				char loc1[128];
                print_source_location(loc1, sizeof(loc1), caller_source_file, caller_source_line);
                snprintf(msg, sizeof(msg), "Sterta zawiera uszkodzony blok pami??ci. Problem zauwa??ono w trakcie wykonywania %s. Opis: %s", loc1, rld_resource_validate_error_message[vderror]);

				report(MSL_FAILURE, RLD_HEAP_BROKEN, NULL, -1, msg);
			}

/*
			sprintf(msg, "vderror=%d; blok utworzony w %s:%d", vderror, only_name(presource->source_file), presource->source_line);

			if (vderror == RVE_INVALID_HEAD_FENCE || RVE_INVALID_TAIL_FENCE)
				report(MSL_FAILURE, RLD_HEAP_DATA_OUT_OF_BOUNDS, caller_source_file, caller_source_line, msg);
*/

			if (vderror == RVE_INVALID_HEAD_FENCE || RVE_INVALID_TAIL_FENCE)
			{
                char loc1[128], loc2[128];
                print_source_location(loc1, sizeof(loc1), presource->source_file, presource->source_line);
                print_source_location(loc2, sizeof(loc2), caller_source_file, caller_source_line);

                snprintf(msg, sizeof(msg), "Uszkodzony zosta?? blok zaalokowany w %s a samo uszkodzenie zauwa??ono w trakcie wykonywania %s. Opis: %s", loc1, loc2, rld_resource_validate_error_message[vderror]);

                report(MSL_FAILURE, RLD_HEAP_DATA_OUT_OF_BOUNDS, NULL, -1, msg);
            }

			assert(!vderror);
			return 0;
		
		}
	}
	//printf("ok");
	return 1;
}


static void __attribute__ ((constructor)) __rldebug_startup()
{
    rldebug_init();
	rldebug_reset_limits();
}

//
//
// RLDebugger 
//
//

void rldebug_reset_limits(void)
{
	//
	rbase.settings.heap.global_limit_active = 0;
	rbase.settings.heap.global_disable = 0;

	for (int command_id = __hfc_min; command_id < __hfc_max; command_id++)
	{
	    struct limit_descriptor_t *plim = &rld_limit[command_id];
	    plim->call_type = command_id;

	    // limity pamieci
//	    plim->heap.general = RLD_HEAP_UNLIMITED;
	    plim->heap.singleshot = RLD_HEAP_UNLIMITED;
	    plim->heap.cumulative.limit = RLD_HEAP_UNLIMITED;
	    plim->heap.cumulative.sum = 0;


	    // limity sukces??w
	    plim->success.limit = RLD_HEAP_UNLIMITED;
	    plim->success.counter = 0;

	}
}

void rldebug_heap_set_global_limit(size_t heap_limit)
{
	if (heap_limit == RLD_HEAP_UNLIMITED)
	{
		// wy????cz ogranicznik
		rbase.settings.heap.global_limit_active = 0;
	} else
	{
		// w????cz 
		rbase.settings.heap.global_limit_value = heap_limit;
		rbase.settings.heap.global_limit_active = 1;
	}
}


void rldebug_heap_disable_all_functions(int disable)
{
	rbase.settings.heap.global_disable = disable;
}


void rldebug_heap_set_function_singleshot_limit(enum heap_function_code_t call_type, size_t limit)
{
	assert(call_type > __hfc_min && call_type < __hfc_max);
	rld_limit[call_type].heap.singleshot = limit;
}

void rldebug_heap_set_function_cumulative_limit(enum heap_function_code_t call_type, size_t limit)
{
	assert(call_type > __hfc_min && call_type < __hfc_max);
	rld_limit[call_type].heap.cumulative.limit = limit;
}


void rldebug_set_function_success_limit(enum heap_function_code_t call_type, size_t limit)
{
	assert(call_type > __hfc_min && call_type < __hfc_max);
	rld_limit[call_type].success.limit = limit;
}


void rldebug_set_reported_severity_level(enum message_severity_level_t lowest_level)
{
	rbase.settings.lowest_reported_severity = lowest_level;
}


//
//
//
//
//

void remove_single_newline(void)
{

	int old_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	fcntl(STDIN_FILENO, F_SETFL, old_flags | O_NONBLOCK);

	int ch = getchar();

	fcntl(STDIN_FILENO, F_SETFL, old_flags);

	if(ch != EOF && ch != '\n')
		ungetc(ch, stdin);
}


int rdebug_call_main(int (*pmain)(int, char**, char**), int argc, char** argv, char** envp)
{
	assert(pmain != NULL);
	//jmp_buf main_return_hook;

	volatile int jstatus = setjmp(rbase.exit_hook);
	int ret_code = 0;
	if (!jstatus)
	{
	    rbase.exit_hooked = 1;

		ret_code = (int8_t)pmain(argc, argv, envp);


		rbase.exit_hooked = 0;
	} else {
	    rbase.exit_hooked = 0;
		assert((jstatus & 0xFF00) == 0x0100);
		ret_code = (int8_t)(jstatus & 0xFF);
	}

    // je??li w buforze klawiatury zapl??ta?? si?? znak nowej linii, to go skasuj
    remove_single_newline();
	return ret_code;
}

