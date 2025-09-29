#ifndef __DMA_EXPORT_H__
#define __DMA_EXPORT_H__

#ifdef __cplusplus
extern "C" {
#endif

	int export_buffer(void* buffer, int size);
	int export_dpu_buffer(void *buffer, int size, int *slice_id_array, int *dpu_array, int num_dpus);


#ifdef __cplusplus
}
#endif

#endif