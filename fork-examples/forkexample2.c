#include <stdio.h>
#include <unistd.h> //fork()
#include <stdlib.h> //exit()
#include <string.h> //strcpy

int gVar = 2;

int main() {
	char pName[80] = "";
	int lVar = 20;
	
   int status;
  
   int pID = fork();
	if(pID == 0) {	//child
		// Code only executed by child process
		strcpy(pName,"Child Process: ");	
		++gVar;
		++lVar;
         
      printf("Starting new process...\n");
      
      char name[6]  = "./fib";
      char input[2] = "8";
      char* argv[3];
      argv[0] = name;
      argv[1] = input;
      argv[2] = NULL;
      
      execvp(argv[0], argv);
	}
	else if(pID < 0) { //failed to fork
		printf("Failed to fork\n");
      exit(1); 
	}
	else {		//parent
		//Code only executed by parent process
      waitpid(pID, &status, 0); 
		strcpy(pName,"Parent Process: ");
	}
	
	//Code executed by both parent and child process
	printf("%s",pName);
   printf("Status was %d\n", status);
	printf("Global Variabile: %d ",gVar);
	printf("Local Variable: %d\n",lVar);
}
