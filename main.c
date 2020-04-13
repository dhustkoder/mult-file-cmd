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
#define CMD_BUFFER_SIZE 512

struct command {
	const char* prog;
	char** args;
	int nargs;
	int phidx;
};

struct directory {
	DIR* dirp;
	const char* path;
	int nfiles;
};

struct command_pool {
	char* cmds;
	int ncmds;
};


static struct directory directory;
static struct command command;
static struct command_pool cmdpools[NTHREADS];
static int files_per_thread;
static int last_thread_leftover;




static bool strcat_next_file_path(char** dest)
{
	struct dirent* entry;

	while ((entry = readdir(directory.dirp)) != NULL) {
		/* check if it is a regular file (not a directory etc...) */
		if (entry->d_type == DT_REG)
			break;
	}

	if (entry != NULL)
		strcat(*dest, entry->d_name);

	return entry != NULL;
}


static bool setup_command_pool(struct command_pool* const pool)
{
	/* allocate enough bytes for all commands in this pool */
	pool->ncmds = pool == &cmdpools[NTHREADS - 1] 
		? files_per_thread + last_thread_leftover
		: files_per_thread;

	const size_t pool_size = sizeof(char) * pool->ncmds * CMD_BUFFER_SIZE + 1;
	pool->cmds = malloc(pool_size);
	if (pool->cmds == NULL) {
		log_error("couldn't enough allocate memory");
		return false;
	}

	/* set all bytes to 0 */
	memset(pool->cmds, 0, pool_size);

	/* add the PRE filename ( $ ) arguments for all commands in this pool */
	char* bufp = pool->cmds;
	for (int i = 0; i < pool->ncmds; ++i) {
		strcpy(bufp, command.prog);
		for (int i = 0; i < command.phidx; ++i) {
			strcat(bufp, " ");
			strcat(bufp, command.args[i]);
		}

		bufp += CMD_BUFFER_SIZE;
	}

	/* add the filenames and the next arguments after the filenames to all commands in this pool */
	bufp = pool->cmds;
	for (int i = 0; i < pool->ncmds; ++i) {
		strcat(bufp, " ");
		strcat(bufp, directory.path);
		strcat(bufp, "/");
		strcat_next_file_path(&bufp);

		for (int i = command.phidx + 1; i < command.nargs; ++i) {
			strcat(bufp, " ");
			strcat(bufp, command.args[i]);
		}

		bufp += CMD_BUFFER_SIZE;
	}

	return true;
}

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
	command.phidx = -1;

	/* find the placeholder index in argument list */
	for (int i = 0; i < command.nargs; ++i) {
		if (strcmp(command.args[i], "$") == 0)
			command.phidx = i;
	}

	if (command.phidx == -1) {
		log_error("file placeholder not found in argument list.\n"
		          "use $ for specifying the file in the command argument list");
		goto Lclosedir;
	}

	files_per_thread = directory.nfiles / NTHREADS;
	last_thread_leftover = directory.nfiles % NTHREADS;

	for (int i = 0; i < NTHREADS - 1; ++i) {
		if (!setup_command_pool(&cmdpools[i])) {
			goto Lclean_pools;
		}
	}

	if (!setup_command_pool(&cmdpools[NTHREADS - 1])) {
		goto Lclean_pools;
	}

	return true;

Lclean_pools:
	for (int i = 0; i < NTHREADS; ++i) {
		if (cmdpools[i].cmds != NULL)
			free(cmdpools[i].cmds);
	}

Lclosedir:
	closedir(directory.dirp);

	return false;
}


static void mfcmd_term(void)
{
	for (int i = 0; i < NTHREADS; ++i) {
		if (cmdpools[i].cmds != NULL)
			free(cmdpools[i].cmds);
	}
	closedir(directory.dirp);
}

static void* command_executer(void* cmdpool_addr)
{
	const struct command_pool* const pool = cmdpool_addr;

	const char* p = pool->cmds;
	for (int i = 0; i < pool->ncmds; ++i) {
		system(p);
		p += CMD_BUFFER_SIZE;
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

