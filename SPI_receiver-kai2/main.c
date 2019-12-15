#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "hwlib.h"
#include "./socal/socal.h"
#include "./socal/hps.h"
#include "./socal/alt_gpio.h"
#include "hps_0.h"

#define HW_REGS_BASE ( ALT_STM_OFST )
#define HW_REGS_SPAN ( 0x04000000 )
#define HW_REGS_MASK ( HW_REGS_SPAN - 1 )
#define EMPTY_WAIT_COUNT 5000000

unsigned char g_buffer[111000000];
int main()
{
	void *virtual_base;
	int fd;
	void *spi_addr;
	void *pio_addr;
	volatile int check_signal;
	volatile int check_pio_signal;
	int i;
	char filename[13];
	unsigned char header[15], checksum;
	int filesize;
	int stime, etime;
	int ptr;
	unsigned char calc_checksum;
	int empty_counter;

	////////////////////////////////////////////////////
	// Initialize
	////////////////////////////////////////////////////
	if( ( fd = open( "/dev/mem", ( O_RDWR | O_SYNC ) ) ) == -1 ) {
		printf( "ERROR: could not open \"/dev/mem\"...\n" );
		return( 1 );
	}

	virtual_base = mmap( NULL, HW_REGS_SPAN, ( PROT_READ | PROT_WRITE ), MAP_SHARED, fd, HW_REGS_BASE );

	if( virtual_base == MAP_FAILED ) {
		printf( "ERROR: mmap() failed...\n" );
		close( fd );
		return( 1 );
	}
	
	spi_addr = virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + SPI_FIFO_0_BASE ) & ( unsigned long)( HW_REGS_MASK ) );
	pio_addr = virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + SPI_PIO_BASE ) & ( unsigned long)( HW_REGS_MASK ) );

	*(uint32_t*)spi_addr = 0x10000; // fifo reset

	////////////////////////////////////////////////////
	// SW wait
	////////////////////////////////////////////////////

	// first of all, waiting for SW pio turning ON...
	printf("waiting for turning on the SW to start salvaging...\n");
	for(;;){
		check_pio_signal = *(uint32_t*)pio_addr;
		if((check_pio_signal & 0x1) == 1){
			break;
		}
		usleep(1000);
	}

	printf("SW wait end -> waiting for empty flag = false\n");
	*(uint32_t*)spi_addr = 0x10000; // fifo reset

	////////////////////////////////////////////////////
	// Receive SPI header infomation
	////////////////////////////////////////////////////
	for(;;){
		check_signal = *(uint32_t*)spi_addr;
		if((check_signal & 0x80000000) == 0) break;
		usleep(1000);
	}
		
	for(i = 0; i < 15; i++){
		check_signal = *(uint32_t*)spi_addr;
		*(uint32_t*)spi_addr = 0x1; //0x80000000; // issue read pulse to fifo
		if((check_signal & 0x80000000) == 0){
			header[i] = check_signal & 0xff;
			//printf("%d fifo:0x%x\n", i, check_signal);
		}else{
			//printf("TERMINAL >> %d fifo:0x%x\n", i, check_signal);
			break;
		}
		usleep(1);
	}	
	
	for(i = 0; i < 8; i++) filename[i] = header[i + 2];
	filename[8] = '.';
	filename[9] = 'd';
	filename[10] = 'a';
	filename[11] = 't';
	filename[12] = NULL;
	filesize = ((header[10] << 24) & 0xff000000) | ((header[11] << 16) & 0xff0000) | ((header[12] << 8) & 0xff00) | (header[13] & 0xff);
	checksum = header[14];
	printf("\n<< HEADER INFO >>\n");
	printf(" - Salvage file name:%s\n", filename);
	printf(" - file size:%d (bytes)\n", filesize);
	printf(" - checksum:0x%x\n", checksum);
		
	////////////////////////////////////////////////////
	// Receive core data
	////////////////////////////////////////////////////
	stime = clock();
	ptr = 0;
	empty_counter = 0;
	for(;;){
//		check_pio_signal = *(uint32_t*)pio_addr;
//		if((check_pio_signal & 0x1) == 1){
		if(empty_counter < EMPTY_WAIT_COUNT){
			check_signal = *(uint32_t*)spi_addr;
			if((check_signal & 0x80000000) == 0){
				// empty flag is false
				g_buffer[ptr] = check_signal & 0xff;
//				if(ptr < 5){
//					printf("%d fifo:0x%x\n", ptr, check_signal);
//				}
				*(uint32_t*)spi_addr = 0x1; //0x80000000; // issue read pulse to fifo
				empty_counter = 0;
				ptr++;
			}else{
				empty_counter++;
			}

			if((check_signal & 0x20000000) != 0){
				printf("!!! Sorry, Full flag is appeared -> stopped !!!\n");
				break;
			}
		}else{
			break;
		}
	}
	etime = clock();
	
	calc_checksum = 0;
	for(i = 0; i < ptr; i++){
		calc_checksum += g_buffer[i];
	}
	
//	printf("\nSalvaging time is %.2f(sec)\n", ((double)etime - (double)stime) / 1000000.0);
	printf("\n<< RECEIVE DATA INFO >>\n");
	printf(" - Received size is %d(bytes)\n", ptr);
	printf(" - Checksum is 0x%x\n", calc_checksum);
	if((calc_checksum == checksum) && (ptr == filesize)) printf(" >>>> MATCH Checksum :)\n");
	else printf(" XXXX unmatch checksum :(\n");

	////////////////////////////////////////////////////
	// Write File
	////////////////////////////////////////////////////
	if((calc_checksum == checksum) && (ptr == filesize)){
		FILE *outputfile;         // 出力ストリーム
		  
		outputfile = fopen(filename, "wb");  // ファイルを書き込み用にオープン(開く)
		if (outputfile == NULL) {          // オープンに失敗した場合
		  printf("cannot open\n");         // エラーメッセージを出して
		  return 1;                         // 異常終了
		}
		  
		fwrite(g_buffer, filesize, 1, outputfile);
		fclose(outputfile);          // ファイルをクローズ(閉じる)
		printf(" >>>> file:%s writing finished\n\n", filename);
	}else{
		printf(" XXXX sorry, can't make salvage file due to unmatching checksum...\n\n");
	}


	////////////////////////////////////////////////////
	// Finalize
	////////////////////////////////////////////////////
	if( munmap( virtual_base, HW_REGS_SPAN ) != 0 ) {
		printf( "ERROR: munmap() failed...\n" );
		close( fd );
		return( 1 );
	}

	close( fd );

	return 1;
}

#if 0
int main() {

	void *virtual_base;
	int fd;
	void *spi_addr;
	volatile int check_signal;
	int i;

	if( ( fd = open( "/dev/mem", ( O_RDWR | O_SYNC ) ) ) == -1 ) {
		printf( "ERROR: could not open \"/dev/mem\"...\n" );
		return( 1 );
	}

	virtual_base = mmap( NULL, HW_REGS_SPAN, ( PROT_READ | PROT_WRITE ), MAP_SHARED, fd, HW_REGS_BASE );

	if( virtual_base == MAP_FAILED ) {
		printf( "ERROR: mmap() failed...\n" );
		close( fd );
		return( 1 );
	}
	
	spi_addr = virtual_base + ( ( unsigned long  )( ALT_LWFPGASLVS_OFST + SPI_FIFO_0_BASE ) & ( unsigned long)( HW_REGS_MASK ) );
	*(uint32_t*)spi_addr = 0x0; // read pulse : write pulse mode
	
	for(i = 0; i < 512; i++){
		check_signal = *(uint32_t*)spi_addr;
		*(uint32_t*)spi_addr = 0x80000000; // issue read pulse to fifo
		if((check_signal & 0x80000000) == 0){
			printf("%d fifo:0x%x\n", i, check_signal);
		}else{
			printf("TERMINAL >> %d fifo:0x%x\n", i, check_signal);
			break;
		}
		usleep(1);
	}	

	*(uint32_t*)spi_addr = 0x10000; // fifo reset
	


	if( munmap( virtual_base, HW_REGS_SPAN ) != 0 ) {
		printf( "ERROR: munmap() failed...\n" );
		close( fd );
		return( 1 );
	}

	close( fd );

	return( 0 );
}
#endif
