/**
 * mix_kernels_ocl.cpp: This file is part of the mixbench GPU micro-benchmark suite.
 *
 * Contact: Elias Konstantinidis <ekondis@gmail.com>
 **/

#include <stdio.h>
//#include <math_constants.h>
#include "loclutil.h"
#include "timestamp.h"

#define COMP_ITERATIONS (8192)
#define UNROLL_ITERATIONS (32)
#define REGBLOCK_SIZE (4)

#define UNROLLED_MEMORY_ACCESSES (UNROLL_ITERATIONS/2)

const int BLOCK_SIZE = 256;
enum KrnDataType{ kdt_int, kdt_float, kdt_double };

char* ReadFile(const char *filename){
	char *buffer = NULL;
	int file_size, read_size;
	FILE *file = fopen(filename, "r");
	if(!file)
		return NULL;
	// Seek EOF
	fseek(file, 0, SEEK_END);
	// Get offset
	file_size = ftell(file);
	rewind(file);
	buffer = (char*)malloc(sizeof(char) * (file_size+1));
	read_size = fread(buffer, sizeof(char), file_size, file);
	buffer[file_size] = '\0';
	if(file_size != read_size) {
		free(buffer);
		buffer = NULL;
	}
	return buffer;
}

double get_event_duration(cl_event ev){
	cl_ulong ev_t_start, ev_t_finish;
	OCL_SAFE_CALL( clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &ev_t_start, NULL) );
	OCL_SAFE_CALL( clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &ev_t_finish, NULL) );
	double time = (ev_t_finish-ev_t_start)/1000000.0;
	return time;
}

/*void initializeEvents(cudaEvent_t *start, cudaEvent_t *stop){
	CUDA_SAFE_CALL( cudaEventCreate(start) );
	CUDA_SAFE_CALL( cudaEventCreate(stop) );
	CUDA_SAFE_CALL( cudaEventRecord(*start, 0) );
}

float finalizeEvents(cudaEvent_t start, cudaEvent_t stop){
	CUDA_SAFE_CALL( cudaGetLastError() );
	CUDA_SAFE_CALL( cudaEventRecord(stop, 0) );
	CUDA_SAFE_CALL( cudaEventSynchronize(stop) );
	float kernel_time;
	CUDA_SAFE_CALL( cudaEventElapsedTime(&kernel_time, start, stop) );
	CUDA_SAFE_CALL( cudaEventDestroy(start) );
	CUDA_SAFE_CALL( cudaEventDestroy(stop) );
	return kernel_time;
}*/

cl_kernel BuildKernel(cl_context context, cl_device_id dev_id, const char *source, const char *parameters){
	cl_int errno;
	const char **sources = &source;
	cl_program program = clCreateProgramWithSource(context, 1, sources, NULL, &errno);
	OCL_SAFE_CALL(errno);
	if( clBuildProgram(program, 1, &dev_id, parameters, NULL, NULL) != CL_SUCCESS ){
		size_t log_size;
		OCL_SAFE_CALL( clGetProgramBuildInfo(program, dev_id, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size) );
		char *log = (char*)alloca(log_size);
		OCL_SAFE_CALL( clGetProgramBuildInfo(program, dev_id, CL_PROGRAM_BUILD_LOG, log_size, log, NULL) );
		OCL_SAFE_CALL( clReleaseProgram(program) );
		fprintf(stderr, "------------------------------------ Kernel compilation log ----------------------------------\n");
		fprintf(stderr, "%s", log);
		fprintf(stderr, "----------------------------------------------------------------------------------------------\n");
		exit(EXIT_FAILURE);
	}
	// Kernel creation
	cl_kernel kernel = clCreateKernel(program, "benchmark_func", &errno);
	OCL_SAFE_CALL(errno);
	return kernel;
}

void ReleaseKernelNProgram(cl_kernel kernel){
	cl_program program_tmp;
	OCL_SAFE_CALL( clGetKernelInfo(kernel, CL_KERNEL_PROGRAM, sizeof(program_tmp), &program_tmp, NULL) );
	OCL_SAFE_CALL( clReleaseKernel(kernel) );
	OCL_SAFE_CALL( clReleaseProgram(program_tmp) );
}

void runbench_warmup(cl_command_queue queue, cl_kernel kernel, cl_mem cbuffer, long size){
	const long reduced_grid_size = size/(UNROLLED_MEMORY_ACCESSES)/32;

	const size_t dimBlock[1] = {BLOCK_SIZE};
	const size_t dimReducedGrid[1] = {(size_t)reduced_grid_size};

	const short seed = 1;
	OCL_SAFE_CALL( clSetKernelArg(kernel, 0, sizeof(cl_short), &seed) );
	OCL_SAFE_CALL( clSetKernelArg(kernel, 1, sizeof(cl_mem), &cbuffer) );

	cl_event event;
	OCL_SAFE_CALL( clEnqueueNDRangeKernel(queue, kernel, 1, NULL, dimReducedGrid, dimBlock, 0, NULL, &event) );
	OCL_SAFE_CALL( clWaitForEvents(1, &event) );
	double runtime = get_event_duration(event);
	OCL_SAFE_CALL( clReleaseEvent( event ) );
printf("debug: runtime %f\n", runtime);
}

template<int memory_ratio>
void runbench(cl_command_queue queue, cl_kernel kernels[kdt_double+1][32+1], cl_mem cbuffer, long size){
	if( memory_ratio>UNROLL_ITERATIONS ){
		fprintf(stderr, "ERROR: memory_ratio exceeds UNROLL_ITERATIONS\n");
		exit(1);
	}

	const long compute_grid_size = size/(UNROLLED_MEMORY_ACCESSES)/2;
	
	const long long computations = 2*(long long)(COMP_ITERATIONS)*REGBLOCK_SIZE*compute_grid_size;
	const long long memoryoperations = (long long)(COMP_ITERATIONS)*compute_grid_size;

	const size_t dimBlock[1] = {BLOCK_SIZE};
	const size_t dimGrid[1] = {(size_t)compute_grid_size};

	cl_event event;
	
	const short seed_f = 1.0f;
	cl_kernel kernel = kernels[kdt_float][memory_ratio];
	OCL_SAFE_CALL( clSetKernelArg(kernel, 0, sizeof(cl_float), &seed_f) );
	OCL_SAFE_CALL( clSetKernelArg(kernel, 1, sizeof(cl_mem), &cbuffer) );
	OCL_SAFE_CALL( clEnqueueNDRangeKernel(queue, kernel, 1, NULL, dimGrid, dimBlock, 0, NULL, &event) );
	OCL_SAFE_CALL( clWaitForEvents(1, &event) );
	double kernel_time_mad_sp = get_event_duration(event);
	OCL_SAFE_CALL( clReleaseEvent( event ) );

	float kernel_time_mad_dp = -1.0;
	float kernel_time_mad_int = -1.0;
//	benchmark_func< float, BLOCK_SIZE, memory_ratio, 0 ><<< dimGrid, dimBlock, 0 >>>(1.0f, (float*)cd);
//	double kernel_time_mad_sp = finalizeEvents(start, stop);

/*	initializeEvents(&start, &stop);
	benchmark_func< double, BLOCK_SIZE, memory_ratio, 0 ><<< dimGrid, dimBlock, 0 >>>(1.0, cd);
	float kernel_time_mad_dp = finalizeEvents(start, stop);

	initializeEvents(&start, &stop);
	benchmark_func< int, BLOCK_SIZE, memory_ratio, 0 ><<< dimGrid, dimBlock, 0 >>>(1, (int*)cd);
	float kernel_time_mad_int = finalizeEvents(start, stop);*/

	const double memaccesses_ratio = (double)(memory_ratio)/UNROLL_ITERATIONS;
	const double computations_ratio = 1.0-memaccesses_ratio;

	printf("      %2d/%2d,     %8.2f,%8.2f,%7.2f,%8.2f,%8.2f,%7.2f,%8.2f,%8.2f,%7.2f\n", 
		UNROLL_ITERATIONS-memory_ratio, memory_ratio,
		kernel_time_mad_sp,
		(computations_ratio*(double)computations)/kernel_time_mad_sp*1000./(double)(1000*1000*1000),
		(memaccesses_ratio*(double)memoryoperations*sizeof(float))/kernel_time_mad_sp*1000./(1000.*1000.*1000.),
		kernel_time_mad_dp,
		(computations_ratio*(double)computations)/kernel_time_mad_dp*1000./(double)(1000*1000*1000),
		(memaccesses_ratio*(double)memoryoperations*sizeof(double))/kernel_time_mad_dp*1000./(1000.*1000.*1000.),
		kernel_time_mad_int,
		(computations_ratio*(double)computations)/kernel_time_mad_int*1000./(double)(1000*1000*1000),
		(memaccesses_ratio*(double)memoryoperations*sizeof(int))/kernel_time_mad_int*1000./(1000.*1000.*1000.) );
}

extern "C" void mixbenchGPU(cl_device_id dev_id, double *c, long size){
#ifdef BLOCK_STRIDED
	const char *benchtype = "compute with global memory (block strided)";
#else
	const char *benchtype = "compute with global memory (grid strided)";
#endif
	printf("Trade-off type:%s\n", benchtype);

	// Set context properties
	cl_platform_id p_id;
	OCL_SAFE_CALL( clGetDeviceInfo(dev_id, CL_DEVICE_PLATFORM, sizeof(p_id), &p_id,	NULL) );

	cl_context_properties ctxProps[] = { CL_CONTEXT_PLATFORM, (cl_context_properties)p_id, 0 };

	cl_int errno;
	// Create context
	cl_context context = clCreateContext(ctxProps, 1, &dev_id, NULL, NULL, &errno);
	OCL_SAFE_CALL(errno);

	cl_mem c_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE, size*sizeof(double), NULL, &errno);
	OCL_SAFE_CALL(errno);
	
	// Create command queue
	cl_command_queue cmd_queue = clCreateCommandQueue(context, dev_id, CL_QUEUE_PROFILING_ENABLE, &errno);
	OCL_SAFE_CALL(errno);

	// Set data on device memory
	cl_int *mapped_data = (cl_int*)clEnqueueMapBuffer(cmd_queue, c_buffer, CL_TRUE, CL_MAP_WRITE, 0, size*sizeof(double), 0, NULL, NULL, &errno);
	OCL_SAFE_CALL(errno);
	for(int i=0; i<size; i++)
		mapped_data[i] = 0;
	clEnqueueUnmapMemObject(cmd_queue, c_buffer, mapped_data, 0, NULL, NULL);

	// Load source, create program and all kernels
	printf("Loading kernel file...\n");
	const char c_param_format_str[] = "-cl-std=CL1.1 -Dclass_T=%s -Dblockdim=%d -Dmemory_ratio=%d -Dgriddim=%ld %s";
	const char *c_block_strided = "-DBLOCK_STRIDED", *c_empty = "";
	char c_build_params[256];
	const char *c_kernel_source = {ReadFile("mix_kernels.cl")};
	printf("Precompilation of kernels...\n");
	sprintf(c_build_params, c_param_format_str, "short", BLOCK_SIZE, 0, 0l, c_block_strided);
	//benchmark_func< short, BLOCK_SIZE, 0, 0 ><<< dimReducedGrid, dimBlock, shared_size >>>((short)1, (short*)cd);
	cl_kernel kernel_warmup = BuildKernel(context, dev_id, c_kernel_source, c_build_params);

	const long compute_grid_size = size/(UNROLLED_MEMORY_ACCESSES)/2;

	cl_kernel kernels[kdt_double+1][32+1];
	for(int i=0; i<=32; i++){
		sprintf(c_build_params, c_param_format_str, "float", BLOCK_SIZE, i, compute_grid_size, c_block_strided);
		printf("%s\n",c_build_params);
		kernels[kdt_float][i] = BuildKernel(context, dev_id, c_kernel_source, c_build_params);
		// ....
	}
	free((char*)c_kernel_source);

	// Synchronize in order to wait for memory operations to finish
	OCL_SAFE_CALL( clFinish(cmd_queue) );

	printf("----------------------------------------- EXCEL data -----------------------------------------\n");
	printf("Operations ratio,  Single Precision ops,,,   Double precision ops,,,     Integer operations,, \n");
	printf("  compute/memory,    Time,  GFLOPS, GB/sec,    Time,  GFLOPS, GB/sec,    Time,   GIOPS, GB/sec\n");

	runbench_warmup(cmd_queue, kernel_warmup, c_buffer, size);

	runbench<32>(cmd_queue, kernels, c_buffer, size);
	runbench<31>(cmd_queue, kernels, c_buffer, size);
	runbench<30>(cmd_queue, kernels, c_buffer, size);
	runbench<29>(cmd_queue, kernels, c_buffer, size);
	runbench<28>(cmd_queue, kernels, c_buffer, size);
	runbench<27>(cmd_queue, kernels, c_buffer, size);
	runbench<26>(cmd_queue, kernels, c_buffer, size);
	runbench<25>(cmd_queue, kernels, c_buffer, size);
	runbench<24>(cmd_queue, kernels, c_buffer, size);
	runbench<23>(cmd_queue, kernels, c_buffer, size);
	runbench<22>(cmd_queue, kernels, c_buffer, size);
	runbench<21>(cmd_queue, kernels, c_buffer, size);
	runbench<20>(cmd_queue, kernels, c_buffer, size);
	runbench<19>(cmd_queue, kernels, c_buffer, size);
	runbench<18>(cmd_queue, kernels, c_buffer, size);
	runbench<17>(cmd_queue, kernels, c_buffer, size);
	runbench<16>(cmd_queue, kernels, c_buffer, size);
	runbench<15>(cmd_queue, kernels, c_buffer, size);
	runbench<14>(cmd_queue, kernels, c_buffer, size);
	runbench<13>(cmd_queue, kernels, c_buffer, size);
	runbench<12>(cmd_queue, kernels, c_buffer, size);
	runbench<11>(cmd_queue, kernels, c_buffer, size);
	runbench<10>(cmd_queue, kernels, c_buffer, size);
	runbench<9>(cmd_queue, kernels, c_buffer, size);
	runbench<8>(cmd_queue, kernels, c_buffer, size);
	runbench<7>(cmd_queue, kernels, c_buffer, size);
	runbench<6>(cmd_queue, kernels, c_buffer, size);
	runbench<5>(cmd_queue, kernels, c_buffer, size);
	runbench<4>(cmd_queue, kernels, c_buffer, size);
	runbench<3>(cmd_queue, kernels, c_buffer, size);
	runbench<2>(cmd_queue, kernels, c_buffer, size);
	runbench<1>(cmd_queue, kernels, c_buffer, size);
	runbench<0>(cmd_queue, kernels, c_buffer, size);

	printf("----------------------------------------------------------------------------------------------\n");

	// Copy results back to host memory
	OCL_SAFE_CALL( clEnqueueReadBuffer(cmd_queue, c_buffer, CL_TRUE, 0, size*sizeof(double), c, 0, NULL, NULL) );
	//CUDA_SAFE_CALL( cudaMemcpy(c, cd, size*sizeof(double), cudaMemcpyDeviceToHost) );

	// Release kernels and program
	ReleaseKernelNProgram(kernel_warmup);
	for(int i=0; i<=32; i++){
		ReleaseKernelNProgram(kernels[kdt_float][i]);
		// ....
	}

	// Release buffer
	OCL_SAFE_CALL( clReleaseMemObject(c_buffer) );
}
