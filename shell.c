#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#define UNDEF 0
#define FG 1
#define BG 2
#define ST 3
#define MAXJOBS 16
#define MAXJID 1<<16
#define MAXLINE 1024
#define READ  0
#define WRITE 1


static char line[1024];
char* username;
char* infile;
char* outfile;
char* cmd[100];
char* cmd1[2];
char* args[512];
pid_t pid;
static int n = 0; /* number of calls to 'command' */
int command_pipe[2];
char arr[100],arr1[100],arr2[100];
int total,inputred,outputred;
char* background;
int verbose = 0;
static pid_t tsh_pid = 0;

int nextjid = 1;            /* next job ID to allocate */

struct job_t {              /* The job struct */
	pid_t pid;              
	int jid;                
	int state;              
	char cmdline[MAXLINE];  
};
struct job_t jobs[MAXJOBS];
void do_bgfg(char **argv);
void waitfg(pid_t pid);
void concatenate(char p[], char q[]); 
int isSubSequence(char str1[], char str2[], int m, int n);
void sigchld_handler(int sig);
void sigint_handler(int sig);
void clearjob(struct job_t *job);
void replaceSubstring(char string[],char sub[],char new[]);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
void listjobs(struct job_t *jobs);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

void modify_job_status(pid_t pid, int job_status)
{
	struct job_t* p_job_entry = getjobpid(jobs, pid);
	p_job_entry->state = job_status;
}
int builtin(char **argv)
{
	if (!strcmp(argv[0], "quit")) {
		int i;
		int stopped = 0;
		for (i=0; i<16; i++) {
			if (jobs[i].state == ST)
				stopped = 1;
		}
		if (stopped == 1) {
			printf("There are stopped jobs\n");
			return 1;

		} else {
			exit(0);
		}
	}
	if (!strcmp(argv[0],"overkill")){
		int i;
		for (i = 0; i < MAXJOBS; i++) {
			if (jobs[i].state == 2) {
				if(killpg(getpgid(jobs[i].pid),9)<0)
					printf("ERROR\n");
			}
		}
		return 1;
	}
	else if(!strcmp(argv[0],"kjob"))								
	{
		int pid;
		if((pid=jid2pid(atoi(args[1])))>=0)
		{
			if(killpg(getpgid(pid),atoi(args[2]))<0)
				perror("Signal not sent!!");
		}
		else
			printf("No such job number\n");
	}
	if (!strcmp(argv[0], "jobs")) {
		listjobs(jobs);
		return 1;
	}
	if (!strcmp(argv[0], "fg") || !strcmp(argv[0], "bg")) {
		do_bgfg(argv);
		return 1;
	}
	if (!strcmp(argv[0], "&"))	/* Ignore singleton & */
		return 1;
	return 0;     /* not a builtin command */
}

void do_bgfg(char **argv) 
{
	int jid, pid;
	char *args = argv[1];
	struct job_t *job;
	if (args != NULL) {     /* if arguments are passed to bg or fg */
			jid = atoi(&args[0]);
			if (!(job = getjobjid(jobs, jid))) {
				printf("%s: No such job\n", args);
				return;
			}
	} 
	else {
		printf("%s command requires PID or %%jobid argument\n", argv[0]);
		return;
	}
	if (job != NULL) {
		pid = job->pid;
		if (job->state == ST) {
			if (!strcmp(argv[0], "bg")) {
				printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
				job->state = BG;
				kill(-pid, SIGCONT);
			}
			if (!strcmp(argv[0], "fg")) {
				job->state = FG;
				kill(-pid, SIGCONT);
				waitfg(job->pid);
			}
		}
		if (job->state == BG) {
			if (!strcmp(argv[0], "fg")) {
				job->state = FG;
				waitfg(job->pid);
			}
		}
	}
	return;
}

void waitfg(pid_t pid)
{
	struct job_t *p = getjobpid(jobs, pid);
	while (p->state == FG)
		sleep(1);
	return;
}
void sigchld_handler(int sig)
{
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0 ) {
		if(WIFEXITED(status)) // if it's child's termination(normally) leading waitpid to return, then delete job
		{
			if(pid == fgpid(jobs)){
				if(tcgetpgrp(STDIN_FILENO) != tsh_pid)
					tcsetpgrp(STDIN_FILENO, tsh_pid);
			}
			deletejob(jobs, pid);
		}
		else if(WIFSIGNALED(status)) // if child terminated abnormally
		{
			if(pid == fgpid(jobs)){
				if(tcgetpgrp(STDIN_FILENO) != tsh_pid)
					tcsetpgrp(STDIN_FILENO, tsh_pid);
			}
			printf("Job [%d] (%d) terminated by signal %d\n",pid2jid(pid),pid,WTERMSIG(status));
			deletejob(jobs, pid);
		}
		else if(WIFSTOPPED(status)) //if the child is stopped, not terminated
		{
			modify_job_status(pid,ST);
			if(pid == fgpid(jobs)){
				if(tcgetpgrp(STDIN_FILENO) != tsh_pid)
					tcsetpgrp(STDIN_FILENO, tsh_pid);
			}
			printf("Job [%d] (%d) stopped by signal %d\n",pid2jid(pid),pid,WSTOPSIG(status));
		}
	}
	if (pid < 0 && errno != ECHILD) {
		printf("waitpid error: %s\n", strerror(errno));
	}
	return;
}

void sigint_handler(int sig)
{
	pid_t pid = fgpid(jobs);
	if (fgpid(jobs) != 0) {
		kill(-pid, SIGINT); 
	}
	return;
}

void sigtstp_handler(int sig)
{
	pid_t pid = fgpid(jobs);
	if (fgpid(jobs) != 0) {
		kill(-pid, SIGTSTP);
	}
	return;
}

void clearjob(struct job_t *job) {
	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}

void initjobs(struct job_t *jobs) {
	int i;
	for (i = 0; i < MAXJOBS; i++)
		clearjob(&jobs[i]);
}

int maxjid(struct job_t *jobs) 
{
	int i, max=0;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid > max)
			max = jobs[i].jid;
	return max;
}

int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
	int i;
	if (pid < 1)
		return 0;
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;
			if (nextjid > MAXJOBS)
				nextjid = 1;
			strcpy(jobs[i].cmdline, cmdline);
			if(verbose){
				printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
			}
			return 1;
		}
	}
	printf("Tried to create too many jobs\n");
	return 0;
}

int deletejob(struct job_t *jobs, pid_t pid) 
{
	int i;
	if (pid < 1)
		return 0;
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			clearjob(&jobs[i]);
			nextjid = maxjid(jobs)+1;
			return 1;
		}
	}
	return 0;
}

pid_t fgpid(struct job_t *jobs) {
	int i;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].state == FG)
			return jobs[i].pid;
	return 0;
}

struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
	int i;
	if (pid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return &jobs[i];
	return NULL;
}

struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
	int i;
	if (jid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			return &jobs[i];
	return NULL;
}

int pid2jid(pid_t pid) 
{
	int i;
	if (pid < 1)
		return 0;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid) {
			return jobs[i].jid;
		}
	return 0;
}

int jid2pid(pid_t jid) 
{
	int i;
	if (jid < 1)
		return 0;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid) {
			return jobs[i].pid;
		}
	return 0;
}

void listjobs(struct job_t *jobs) 
{
	int i;
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid != 0) {
			printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
			if(jobs[i].state ==  BG) 
				/*printf("Running ")*/;
			else if(jobs[i].state ==  FG) 
				printf("Foreground ");
			else if(jobs[i].state == ST)
				printf("Stopped ");
			else
				printf("listjobs: Internal error: job[%d].state=%d ",i, jobs[i].state);
			printf("%s\n", jobs[i].cmdline);
		}
	}
}

handler_t *Signal(int signum, handler_t *handler) 
{
	struct sigaction action, old_action;
	action.sa_handler = handler;  
	sigemptyset(&action.sa_mask); /* block sigs of type being handled */
	action.sa_flags = SA_RESTART; /* restart syscalls if possible */

	if (sigaction(signum, &action, &old_action) < 0)
		printf("Signal error\n");
	return (old_action.sa_handler);
}

void sigquit_handler(int sig) 
{
	printf("Terminating after receipt of SIGQUIT signal\n");
	exit(1);
}

static int command(int input, int first, int last)
{
	int pipettes[2];
	FILE *fp;
	int bg;
	char buf[1024];
	pid_t pid;
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask,SIGCHLD);
	strcpy(buf,line);
	if(!builtin(args)){
		sigprocmask(SIG_BLOCK, &mask, 0);
		if (strcmp(args[0],"cd")==0){
			int f;
			if(args[1] == NULL){
				printf("ERROR : Argument not found.\n");
				return 1;
			}
			if(strcmp(args[1],"~")==0){
				f = chdir(arr2);
			}
			else{
				f= chdir(args[1]);
			}
			if(f != 0){
				printf("ERROR : No such Directory.\n");
			}
		}
		else {
			pipe( pipettes );	
			pid = fork();

			if (pid == 0) {
				if(inputred){
					fp = fopen(infile,"r");
					dup2(fileno(fp),0);
				}
				if(outputred){
					fp = fopen(outfile,"w+");
					dup2(fileno(fp),1);
				}
				if (first == 1 && last == 0 && input == 0) {
					dup2( pipettes[WRITE], STDOUT_FILENO );
				} else if (first == 0 && last == 0 && input != 0) {
					dup2(input, STDIN_FILENO);
					dup2(pipettes[WRITE], STDOUT_FILENO);
				} else {
					dup2( input, STDIN_FILENO );
				}
				setpgid(0,0);
				if (execvp( args[0], args) == -1){
					_exit(EXIT_FAILURE); // If child fails
				}

			}
			else if(pid > 0){
				if(background){
					addjob(jobs,pid,BG,line);
					printf("[%d] (%d) %s\n",pid2jid(pid),pid,line);
					int status,x;
				}
				else{
					addjob(jobs,pid,FG,line);
				}
				sigprocmask(SIG_UNBLOCK,&mask,0);
				waitfg(pid);
			}
			else {
				printf("fork error\n");
			}
			if (input != 0) 
				close(input);

			close(pipettes[WRITE]);
			if (last == 1)
				close(pipettes[READ]);
		}
	}
	return pipettes[READ];
}

static void cleanup(int n)
{
	int i;
	for (i = 0; i < n; ++i) 
		wait(NULL); 
}


static void split(char* cmd);

static int run(char* cmd, int input, int first, int last)
{
	inputred = 0;
	outputred = 0;
	split(cmd);
	if (args[0] != NULL) {
		n += 1;
		int re = command(input, first, last);
		return re;
	}
	return 0;
}

void breakstatement(char* stat,char** cmd){                                                          //To break input string into various commands 
	char *x;
	x = strtok(stat, ";");
	while(1){
		cmd[total] = x;
		total++;
		x = strtok(NULL, ";");
		if(x==NULL)
			break;
	}
}


static char* skipwhite(char* s)
{
	while (isspace(*s)) ++s;
	return s;
}

static void split(char* cmd){
	cmd = skipwhite(cmd);
	char* next1 = strchr(cmd, '>');
	char* next2 = strchr(cmd, '<');
	int i =0;
	if(next1 || next2){
		char* next = strchr(cmd, ' ');
		i = 0;
		int flash1 = 0,flash2 = 0;
		while(next != NULL) {
			next[0] = '\0';
			if(strcmp(cmd,"&")==0)
			{
				;
			}
			else{
				if(strcmp(cmd,">")==0){
					outputred = 1;
					flash2 = 1;
				}
				else{
					if(strcmp(cmd,"<")==0){
						inputred = 1;
						flash1 = 1;
					}
					else {
						if(flash1 == 1){
							infile = cmd;
							if(strcmp(infile,"\0")){
								strcat(infile,"\0");
							}
							flash1 = 0;
						}
						else if(flash2 == 1){
							outfile = cmd;
							if(!strcmp(outfile,"\0"))
								strcat(outfile,"\0");
							flash2 = 0;
						}
						else{		
							args[i] = cmd;
							++i;
						}
					}
				}
			}
			cmd = skipwhite(next + 1);
			next = strchr(cmd, ' ');
		}

		if (cmd[0] != '\0') {
			if(flash1 == 1){
				infile = cmd;
				if(!strcmp(infile,"\0"))
					strcat(infile,"\0");
				flash1 = 0;
			}
			else if(flash2 == 1){
				outfile = cmd;
				if(!strcmp(outfile,"\0"))
					strcat(outfile,"\0");
				flash2 = 0;
			}
			else{		
				args[i] = cmd;
				++i; 
			}
			next = strchr(cmd, '\n');
			next[0] = '\0';
		}
		args[i]=NULL;
	}
	else {
		char* next = strchr(cmd, ' ');
		i = 0;
		while(next != NULL) {
			next[0] = '\0';
			if(strcmp(cmd,"&")==0)
			{
				;
			}
			else{
				args[i] = cmd;
				++i;
			}
			cmd = skipwhite(next + 1);
			next = strchr(cmd, ' ');
		}

		if (cmd[0] != '\0') {
			args[i] = cmd;
			next = strchr(cmd, '\n');
			next[0] = '\0';
			++i; 
		}
		args[i] = NULL;
	}
}

int main()
{
	Signal(SIGCHLD, sigchld_handler);
	Signal(SIGINT,  sigint_handler);   /* ctrl-c */
	Signal(SIGTSTP, sigtstp_handler); 
	Signal(SIGTERM, sigtstp_handler);
	Signal(SIGQUIT, sigquit_handler); 
	Signal(SIGTTOU, SIG_IGN);
	Signal(SIGTTIN, SIG_IGN);
	initjobs(jobs);
	getcwd(arr2,100);
	while (1) {
		getcwd(arr1,100);
		gethostname(arr, 100);
		username = getenv("USER");
		char *s;
		s = (char *)malloc(100*sizeof(char));
		char* q = "/home/";
		char* w = "~";
		concatenate(s,q);
		concatenate(s,username);
		if(isSubSequence(arr1,s,strlen(arr1),strlen(s))){
			replaceSubstring(arr1,s,w);
		}
		printf("%s@%s:%s $ ",username,arr,arr1);

		fflush(NULL);
		if (!fgets(line, 1024, stdin)) 
			return 0;
		int input = 0;
		int first = 1;
		char* stat = line;
		background = strchr(stat, '&');
		if(background){
			*background = ' ';
		}
		int i;
		breakstatement(stat,cmd);
		for(i=0;i<total;i++){
			char* next = strchr(cmd[i], '|'); /* Find '|' */

			while (next != NULL) {
				*next = '\0';
				input = run(cmd[i], input, first, 0);
				cmd[i]=NULL;


				cmd[i] = next + 1;
				next = strchr(cmd[i], '|'); 
				first = 0;
			}
			input = run(cmd[i], input, first, 1);
			cmd[i]=NULL;
		}
		total = 0;
		n = 0;
	}
	return 0;
}


void replaceSubstring(char string[],char sub[],char new[])
{
	int sLen,Len,nLen;
	int i=0,j,k;
	int flag=0,start,end;
	sLen=strlen(string);
	Len=strlen(sub);
	nLen=strlen(new);

	for(i=0;i<sLen;i++)
	{
		flag=0;
		start=i;
		for(j=0;string[i]==sub[j];j++,i++) 
			if(j==Len-1)
				flag=1; 
		end=i;
		if(flag==0)
			i-=j;
		else
		{
			for(j=start;j<end;j++) 
			{
				for(k=start;k<sLen;k++)
					string[k]=string[k+1];
				sLen--;
				i--;
			}

			for(j=start;j<start+nLen;j++)    
			{
				for(k=sLen;k>=j;k--)
					string[k+1]=string[k];
				string[j]=new[j-start];
				sLen++;
				i++;
			}
		}
	}
}

void concatenate(char p[], char q[]) {
	int c, d;
	c = 0;
	while (p[c] != '\0') {
		c++;
	}
	d = 0;
	while (q[d] != '\0') {
		p[c] = q[d];
		d++;
		c++;
	}
	p[c] = '\0';
}

int isSubSequence(char str1[], char str2[], int m, int n)
{
	if (m == 0) 
		return 1;
	if (n == 0) 
		return 0;
	if (str1[m-1] == str2[n-1])
		return isSubSequence(str1, str2, m-1, n-1);
	return isSubSequence(str1, str2, m-1, n);
}

