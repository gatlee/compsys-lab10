/*************************************
 
 Demo for pthread commands
 compile: gcc threadX.c -o threadX -lpthread
 
***************************************/

#include <stdio.h>
#include <pthread.h>

void *say_hello(void *param);  /* the work_function */

int main(int args, char **argv) 
{
	pthread_t tid; /* thread identifier */
   
	/* create the thread */
	pthread_create(&tid, NULL, say_hello, NULL);
	
	/* wait for thread to exit */ 
	pthread_join(tid, NULL);

	printf("Hello from first thread\n");   
	return 0;
}

void *say_hello(void *param) 
{
	printf("Hello from second thread\n");	
	return NULL;
}

