#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>


#define log(...) do { fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while (0)
#define log_error(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

#define NTHREADS 4
#define CMD_BUFFER_SIZE 1024
#define FILEPATHS_INIT_SIZE (1024 * 16)

struct command {
	const char* prog;
	char** args;
	int nargs;
};

struct directory {
	DIR* dirp;
	const char* path;
	int nfiles;
};

struct command_pool {
	int nfiles;
	short* filepathlens;
	char* filepaths;
	char cmdbuffer[CMD_BUFFER_SIZE];
};


static struct directory directory;
static struct command command;
static struct command_pool cmdpools[NTHREADS];
static int files_per_thread;
static int last_thread_leftover;


static bool strcpy_next_file_path(char* dest)
{
	struct dirent* entry;

	while ((entry = readdir(directory.dirp)) != NULL) {
		/* check if it is a regular file (not a directory etc...) */
		if (entry->d_type == DT_REG)
			break;
	}

	if (entry != NULL)
		strcpy(dest, entry->d_name);

	return entry != NULL;
}


static bool setup_command_pool(
		struct command_pool* const pool,
		const int nfiles
)
{
	pool->nfiles = nfiles;
	pool->filepathlens = malloc(sizeof(*pool->filepathlens) * pool->nfiles);
	if (pool->filepathlens == NULL) {
		log_error("coulndn't allocate enough memory");
		return false;
	}
	pool->filepaths = malloc(sizeof(*pool->filepaths) * FILEPATHS_INIT_SIZE);
	if (pool->filepaths == NULL) {
		log_error("couldn't allocate enough memory");
		free(pool->filepathlens);
		pool->filepathlens = NULL;
		return false;
	}

	strcpy(pool->cmdbuffer, command.prog);
	for (int i = 0; i < command.nargs - 1; ++i) {
		strcat(pool->cmdbuffer, " ");
		strcat(pool->cmdbuffer, command.args[i]);
	}
	strcat(pool->cmdbuffer, " ");
	strcat(pool->cmdbuffer, directory.path);
	strcat(pool->cmdbuffer, "/");

	char filepath[512] = { 0 };
	size_t filepaths_data_size = 0;
	size_t filepaths_buffer_size = FILEPATHS_INIT_SIZE;
	char* filepaths_writer = pool->filepaths;
	for (int i = 0; i < pool->nfiles; ++i) {
		strcpy_next_file_path(filepath);
		const int filepathlen = strlen(filepath);
		pool->filepathlens[i] = filepathlen;
		filepaths_data_size += filepathlen;
		if (filepaths_data_size > filepaths_buffer_size) {
			const size_t newsize = filepaths_data_size * 2;
			const size_t writer_pos = filepaths_writer - pool->filepaths;
			pool->filepaths = realloc(pool->filepaths, newsize); 
			if (pool->filepaths == NULL) {
				log_error("couldn't allocate enough memory");
				free(pool->filepaths);
				free(pool->filepathlens);
				pool->filepaths = NULL;
				pool->filepathlens = NULL;
				return false;
			}
			filepaths_buffer_size = newsize;
			filepaths_writer = pool->filepaths + writer_pos;
		}
		memcpy(filepaths_writer, filepath, filepathlen);
		filepaths_writer += filepathlen;
	}

	pool->filepaths = realloc(pool->filepaths, filepaths_data_size);

	return true;
}

static void mfcmd_term(void);

static bool mfcmd_init(int argc, char** argv)
{
	/* alloc aux buffer on stack */
	command.prog = argv[2];
	char buffer[strlen("which ") + strlen(command.prog) + 1];


	/* test if directory is valid */
	directory.path = argv[1];
	directory.dirp = opendir(directory.path);
	if (!directory.dirp) {
		log_error("invalid directory %s", directory.path);
		return false;
	}

	/* get how many files */
	struct dirent* entry = NULL;
	while ((entry = readdir(directory.dirp)) != NULL) {
		if (entry->d_type == DT_REG)
			++directory.nfiles;
	}

	rewinddir(directory.dirp);

	if (directory.nfiles == 0) {
		log_error("no files in directory %s", directory.path);
		goto Lclosedir;
	}
	
	/* test if command's program is valid */
	strcpy(buffer, "which ");
	strcat(buffer, command.prog);

	if (system(buffer) != 0) {
		log_error("invalid command");
		goto Lclosedir;
	}

	command.args = argv + 3;
	command.nargs = argc - 3;

	int phidx = -1;
	/* find the placeholder index in argument list */
	for (int i = 0; i < command.nargs; ++i) {
		if (strcmp(command.args[i], "$") == 0)
			phidx = i;
	}

	if (phidx != command.nargs - 1) {
		log_error(
			"file placeholder not found in argument list.\n"
			"use $ for specifying the file in the command argument list.\n"
			"$ should be the last argument in argument list."
		);
		goto Lclosedir;
	}

	files_per_thread = directory.nfiles / NTHREADS;
	last_thread_leftover = directory.nfiles % NTHREADS;

	for (int i = 0; i < NTHREADS - 1; ++i) {
		if (!setup_command_pool(&cmdpools[i], files_per_thread)) {
			goto Lclean_pools;
		}
	}

	if (!setup_command_pool(&cmdpools[NTHREADS - 1], files_per_thread + last_thread_leftover)) 
		goto Lclean_pools;

	return true;

Lclean_pools:
Lclosedir:
	mfcmd_term();
	return false;
}


static void mfcmd_term(void)
{
	for (int i = 0; i < NTHREADS; ++i) {
		if (cmdpools[i].filepaths != NULL)
			free(cmdpools[i].filepaths);
		if (cmdpools[i].filepathlens != NULL)
			free(cmdpools[i].filepathlens);
	}
	if (directory.dirp != NULL)
		closedir(directory.dirp);
}

static void* command_executer(void* cmdpool_addr)
{
	struct command_pool* const pool = cmdpool_addr;

	char* cmdbuffer_write_pos = pool->cmdbuffer + strlen(pool->cmdbuffer);
	const char* filepaths_reader = pool->filepaths;
	for (int i = 0; i < pool->nfiles; ++i) {
		memcpy(
			cmdbuffer_write_pos,
			filepaths_reader,
			pool->filepathlens[i]
		);
		*(cmdbuffer_write_pos + pool->filepathlens[i]) = '\0';
		filepaths_reader += pool->filepathlens[i];
		system(pool->cmdbuffer);
	}

	return NULL;
}

static void mfcmd_run(void)
{
	log(
		"INFO: \n"
		"DIRECTORY: %s\n"
		"NUMBER OF FILES TO PROCESS: %d\n"
		"NUMBER OF THREADS: %d\n"
		"FILES PER THREAD: %d + %d ON LAST THREAD\n",
		directory.path,
		directory.nfiles,
		NTHREADS,
		files_per_thread,
		last_thread_leftover
	);
	
	log("press y to confirm and RUN");

	char c = getchar();

	if (c !='y')
		return;

	pthread_t thr[NTHREADS - 1];

	for (int i = 0; i < NTHREADS - 1; ++i)
		pthread_create(&thr[i], NULL, &command_executer, &cmdpools[i]);

	command_executer(&cmdpools[NTHREADS - 1]);

	for (int i = 0; i < NTHREADS - 1; ++i)
		pthread_join(thr[i], NULL);
}



int main(int argc, char** argv)
{
	if (argc < 3) {
		log_error("Usage: %s [directory] [command] ...command args with $ for file placeholder", argv[0]);
		return EXIT_FAILURE;
	}

	if (!mfcmd_init(argc, argv))
		return EXIT_FAILURE;


	mfcmd_run();

	mfcmd_term();
	return EXIT_SUCCESS;
}

