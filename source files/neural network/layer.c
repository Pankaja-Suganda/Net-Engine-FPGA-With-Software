#include "layer.h"
#include "channels.h"
#include "net_engine.h"
#include "net_engine_hw.h"
#include "xscugic.h"
#include "xparameters.h"
#include <stdlib.h>
#include <xil_printf.h>
#include <math.h>
#include "neural_network.h"

#define PROCESS_TIME_MEASURE

#ifdef PROCESS_TIME_MEASURE
#include "time_measure.h"
#endif

#include <float.h>
#define NET_ENGINE_1_AXI_DMA_BASEADDR XPAR_AXI_DMA_0_BASEADDR
#define NET_ENGINE_1_CONFIG_BASEADDR  XPAR_NET_ENGINE_0_BASEADDR

#define max(a, b) ((a) > (b) ? (a) : (b))

Layer* LAYER_init(LAYER_TYPE type, LAYER_ACTIVATION activation, u32* memory_ptr, u32 memory_len){
    Layer *instance; 

    instance = (Layer *)malloc(sizeof(Layer));
    if (instance == NULL) {
        printf("Layer malloc failed\n");
        return NULL;
    }

    instance->index                     = 0;
    instance->state                     = LAYER_STATE_NOT_STARTED;
    instance->type                      = type;
    instance->func.post_process         = NULL;
    instance->func.pre_process          = NULL;
    instance->input_channels.channels   = NULL;
    instance->output_channels.channels  = NULL;
    instance->input_channels.count      = 0;
    instance->output_channels.count     = 0;
    instance->ptr.input                 = NULL;
    instance->ptr.output                = NULL;
    instance->ptr.temp                  = NULL;
    instance->memory.memory_ptr         = memory_ptr;
    instance->memory.memory_tail        = memory_ptr;
    instance->memory.availale_mem_size  = memory_len;
    instance->memory.used_mem_size      = 0;
    instance->activation                = activation;

    switch (type) {
        case LAYER_TYPE_CNN_3X3:    instance->data.cnn_data.data     = NULL; break;
        case LAYER_TYPE_MAXPOOLING: instance->data.mx_data.data      = NULL; break;
    };

    return instance;
}

static Channel_Node* create_channel_node(Channel data){
    Channel_Node* new = (Channel_Node*)malloc(sizeof(Channel_Node));
    if(new == NULL){
        xil_printf("Node malloc error \r\n");
        return NULL;
    }
    new->data = data;
    new->next = NULL;

    return new;
}

static void append_channel_node(Channel_Node** head_ref, Channel new_data) {
    Channel_Node* new = create_channel_node(new_data);
    if (new == NULL) {
        return;
    }

    if (*head_ref == NULL) {
        *head_ref = new;
        return;
    }

    Channel_Node* last = *head_ref;
    while (last->next != NULL) {
        last = last->next;
    }

    last->next = new;
}

int LAYER_add_input_channel(Layer *instance, u32 height, u32 width, u32 *input_ptr){
    Channel *channel = NULL;

    channel = CHANNEL_init(CHANNEL_TYPE_INPUT, height, width, input_ptr);
    if(channel == NULL){
        return -1;
    }

    channel->index = instance->input_channels.count;
    channel->activation = instance->activation;
    if(instance->input_channels.channels == NULL){
        instance->input_channels.channels = create_channel_node(*channel);
    }
    else{
        append_channel_node(&(instance->input_channels.channels), *channel);
    }
    instance->input_channels.count++;

    return 0;
}

int LAYER_add_maxpool_output_channels(Layer **instance, u32 pool_size, u32 stride, u32 padding, u32 channel_count, u32 height, u32 width){
    Channel *channel;

    if (instance == NULL) {
        return -1; // Invalid arguments
    }

    for(int chan = 0; chan < channel_count; chan++){
        channel = CHANNEL_init(CHANNEL_TYPE_OUTPUT, height, width, NULL);
        if(channel == NULL){
            return -1;
        }
        channel->total_bytes = height * width;
        channel->input_ptr  = NULL;
        channel->output_ptr = (*instance)->memory.memory_tail;
        channel->activation = (*instance)->activation;
        channel->data.mx_data.padding   = padding;
        channel->data.mx_data.pool_size = pool_size;
        channel->data.mx_data.stride    = stride;

        (*instance)->memory.memory_tail       += channel->total_bytes;
        (*instance)->memory.availale_mem_size -= channel->total_bytes;
        (*instance)->memory.used_mem_size     += channel->total_bytes;


        channel->index = (*instance)->output_channels.count;
        if((*instance)->output_channels.channels == NULL){
            (*instance)->output_channels.channels = create_channel_node(*channel);
        }
        else{
            append_channel_node(&((*instance)->output_channels.channels), *channel);
        }
        (*instance)->output_channels.count++;
    }
}

int LAYER_add_cnn_1x1_output_channels(Layer **instance, void *weights, void *bias, int weights_count, int channel_count, u32 height, u32 width){
    Channel *channel;
    Channel_Node *input_channel;
    CNN_1x1_Data *data_ptr;

    u32 *weights_ptr = (u32*)weights;
    u32 *bias_ptr    = (u32*)bias;

    u32 kernal_data_count =  weights_count/channel_count;

    if ((*instance) == NULL || weights_ptr == NULL || bias_ptr == NULL) {
        return -1;
    }

    for(int chan = 0; chan < channel_count; chan++){
        channel = CHANNEL_init(CHANNEL_TYPE_OUTPUT, height, width, NULL);
        if(channel == NULL){
            return -1;
        }

        channel->total_bytes = height * width;
        channel->input_ptr  = NULL;
        channel->output_ptr = (*instance)->memory.memory_tail;
        channel->activation = LAYER_ACTIVATION_NOT_REQUIRED;

        (*instance)->memory.memory_tail       += channel->total_bytes;
        (*instance)->memory.availale_mem_size -= channel->total_bytes;
        (*instance)->memory.used_mem_size     += channel->total_bytes;

        data_ptr = (CNN_1x1_Data*) malloc(sizeof(CNN_1x1_Data)); 
        if(data_ptr == NULL){
            return -1;
        }

        data_ptr->kernal_data  = (u32*)&weights_ptr[kernal_data_count * chan];
        data_ptr->bias         = *(float*)&bias_ptr[chan];
        data_ptr->kernal_count = kernal_data_count;

        // printf("kernal data Channel(%d) B(%f) KC(%d) \n", channel->index, *(float*)&data_ptr->bias, data_ptr->kernal_count);

        channel->cnn_1x1_data.data = data_ptr;
        channel->index = (*instance)->output_channels.count;
        if((*instance)->output_channels.channels == NULL){
            (*instance)->output_channels.channels = create_channel_node(*channel);
        }
        else{
            append_channel_node(&((*instance)->output_channels.channels), *channel);
        }
        (*instance)->output_channels.count++;
    }
}

int LAYER_add_cnn_output_channels(Layer **instance, void* ptr, void* ptr_activation, int channel_count, u32 height, u32 width){
    Channel *channel;
    Channel_Node *input_channel;
    float value = 0.0;

    u8   kernal_count = 0;
    u32* activation   = (u32*) ptr_activation;

    Channel_Kernal_Data *kernal_ptr = (Channel_Kernal_Data*)ptr;

    if (instance == NULL || ptr == NULL) {
        return -1; // Invalid arguments
    }

    for(int chan = 0; chan < channel_count; chan++){
        channel = CHANNEL_init(CHANNEL_TYPE_OUTPUT, height, width, NULL);
        if(channel == NULL){
            return -1;
        }
        value = *(float*)&activation[chan];

        channel->total_bytes = height * width;
        channel->input_ptr  = NULL;
        channel->output_ptr = (*instance)->memory.memory_tail;
        channel->activation = (*instance)->activation;
        if((*instance)->activation == LAYER_ACTIVATION_RELU){
            channel->data.relu_data.alpha  = value;
        }

        (*instance)->memory.memory_tail       += channel->total_bytes;
        (*instance)->memory.availale_mem_size -= channel->total_bytes;
        (*instance)->memory.used_mem_size     += channel->total_bytes;

        input_channel = (*instance)->input_channels.channels;
        kernal_count  = 0;

        while(input_channel != NULL){
            if((*instance)->type == LAYER_TYPE_CNN_3X3){
                CHANNEL_load_kernal(channel, kernal_ptr[(chan * (*instance)->input_channels.count) + kernal_count], &(input_channel->data));
                kernal_count++;
            }
            
            input_channel = input_channel->next;
        }

        channel->index = (*instance)->output_channels.count;
        if((*instance)->output_channels.channels == NULL){
            (*instance)->output_channels.channels = create_channel_node(*channel);
        }
        else{
            append_channel_node(&((*instance)->output_channels.channels), *channel);
        }
        (*instance)->output_channels.count++;
    }
}

int LAYER_link(Layer *input_layer, Layer *output_layer){
    Channel_Node *channel = NULL;
    if(input_layer == NULL || output_layer == NULL){
        return -1;
    }

    output_layer->input_channels.channels = input_layer->output_channels.channels;
    output_layer->input_channels.count    = input_layer->output_channels.count;

    channel = output_layer->input_channels.channels;
    while(channel != NULL){
        channel->data.type  = CHANNEL_TYPE_INPUT;
        channel->data.state = CHANNEL_STATE_NOT_STARTED;

        channel = channel->next;
    }

    return 0;
}
// int LAYER_setup_net_engine(Net_Engine_Inst *instance){
//     NET_STATUS Status;

//     // net engine initializing
//     Status = NET_ENGINE_init(instance, NET_ENGINE_1_CONFIG_BASEADDR, NET_ENGINE_1_AXI_DMA_BASEADDR);
//     if(Status != NET_ENGINE_OK){
//         xil_printf("Net engine init failed\n");
//     }

//     // intterupt setup
//     Status = NET_ENGINE_intr_setup(instance, XPAR_XSCUGIC_0_BASEADDR);
//     if(Status != NET_ENGINE_OK){
//         xil_printf("Net engine interrupt setup failed\n");
//     }

//     // register interrupts
//     Status = NET_ENGINE_register_intr(instance, NET_ENGINE_RECEIVE_INTR, XPS_FPGA1_INT_ID);
//     if(Status != NET_ENGINE_OK){
//         xil_printf("Net engine register NET_ENGINE_RECEIVE_INTR failed\n");
//     }

//     Status = NET_ENGINE_register_intr(instance, NET_ENGINE_ROW_COMPLETE_INTR, XPS_FPGA2_INT_ID);
//     if(Status != NET_ENGINE_OK){
//         xil_printf("Net engine register NET_ENGINE_ROW_COMPLETE_INTR failed\n");
//     }
//     return 0;
// }

// int LAYER_Max_pooling_set_data(Max_Pooling_Layer *instance, u32* data, u16 count){
//     xil_printf("LAYER_Max_pooling_set_data \n");
//     return 0;
// }

// int LAYER_Max_pooling_init(Max_Pooling_Layer *instance, u32* input, u32* output){
//     int ret = 0;

//     // set up max poolng details
//     instance->completed_count = 0;
//     instance->total_count     = 0;

//     // set up layer details
//     instance->layer.index  = 1;
//     instance->layer.input  = input;
//     instance->layer.output = output;
//     instance->layer.temp   = output;
//     instance->layer.state  = LAYER_STATE_NOT_STARTED;

//     // set up net engine
//     ret = LAYER_setup_net_engine(&(instance->layer.net_engine));

//     return ret;
// }

int LAYER_MAXPOOLING_process(Layer *instance){

    u32 in_height = 0;
    u32 in_width  = 0;
    u32 out_height = 0;
    u32 out_width  = 0;
    u32 stride = 0;
    u32 size   = 0;
    u32 y_out  = 0;
    u32 x_out  = 0;
    u32 start_x = 0;
    u32 start_y = 0;

    float max_value = 0.0;

    Channel_Node *input_channel  = instance->input_channels.channels;
    Channel_Node *output_channel = instance->output_channels.channels;

    if(input_channel == NULL || output_channel == NULL){
        return -1;
    }

    in_height  = input_channel->data.height;
    in_width   = input_channel->data.width;
    out_height = output_channel->data.height;
    out_width  = output_channel->data.width;
    stride = output_channel->data.data.mx_data.stride;
    size   = output_channel->data.data.mx_data.pool_size;

    while(input_channel != NULL){
        for (int Index = 0; Index < output_channel->data.total_bytes; Index++) {
            output_channel->data.output_ptr[Index] = -FLT_MAX;
        }

        for(y_out = 0; y_out < out_height; y_out++){
            for(x_out = 0; x_out < out_width; x_out++){
                max_value = -1e10;
                max_value = -FLT_MAX;
                
                start_y = y_out * stride;
                start_x = x_out * stride;

                // Iterate over the pooling window
                for (u32 ky = 0; ky < size; ky++) {
                    for (u32 kx = 0; kx < size; kx++) {
                        u32 iy = start_y + ky;
                        u32 ix = start_x + kx;
                        
                        // Check bounds
                        if (iy < in_height && ix < in_width) {
                            float value = *(float*)&input_channel->data.output_ptr[iy * in_width + ix];
                            if (value > max_value) {
                                max_value = value;
                            }
                        }
                    }
                }
                
                // Assign the maximum value to the output feature map
                output_channel->data.output_ptr[y_out * out_width + x_out] = *(u32*)&max_value;
            }
        }


        // for (int Index = 0; Index < 10; Index++) {
        //     printf("\t %d maxp %f \\r\n", Index, *(float*)&output_channel->data.output_ptr[Index]);
        // }
        // xil_printf("\tchannel %d completed \r\n", input_channel->data.index);
        input_channel  = (Channel_Node *)input_channel->next;
        output_channel = (Channel_Node *)output_channel->next;
    }



    return 0;
}

// int LAYER_RELU_process(Layer *instance){
//     int ret = 0;

//     return ret;
// }

// int LAYER_SOFTMAX_process(Layer *instance){
//     int ret = 0;
    
//     return ret;
// }

// int LAYER_CNN_init(CNN_Layer *instance, LAYER_TYPE type, u32* memory_ptr, u32 memory_len){
//     int ret = 0;

//     // set up max poolng details
//     instance->completed_count = 0;
//     instance->total_count     = 0;

//     // set up layer details
//     instance->layer.index           = 1;
//     instance->layer.state           = LAYER_STATE_NOT_STARTED;
//     instance->layer.type            = type;
//     instance->data                  = NULL;
//     instance->layer.post_process    = NULL;
//     instance->layer.pre_process     = NULL;
//     instance->input_channels        = NULL;
//     instance->output_channels       = NULL;
//     instance->input_channels_count  = 0;
//     instance->output_channels_count = 0;
//     instance->memory_ptr            = memory_ptr;
//     instance->availale_mem_size     = memory_len;
//     instance->used_mem_size         = 0;

//     // set up net engine
//     ret = LAYER_setup_net_engine(&(instance->layer.net_engine));

//     return ret;
// }

// void LAYER_CNN_set_callbacks(CNN_Layer *instance, Layer_Data_Post_Process *post_process, Layer_Data_Pre_Process  *pre_process){
//     instance->layer.post_process = post_process;
//     instance->layer.pre_process  = pre_process;
// }



// CNN_Data_Node* create_data_node(CNN_Config_Data data){
//     CNN_Data_Node* new = (CNN_Data_Node*)malloc(sizeof(CNN_Config_Data));
//     if(new == NULL){
//         xil_printf("Node malloc error %d \r\n", data.index);
//         return NULL;
//     }
//     new->config_data = data;
//     new->next        = NULL;

//     return new;
// }

// void append_node(CNN_Data_Node** head_ref, CNN_Config_Data new_data) {
//     CNN_Data_Node* new = create_data_node(new_data);
//     if (new == NULL) {
//         return;
//     }

//     if (*head_ref == NULL) {
//         *head_ref = new;
//         return;
//     }

//     CNN_Data_Node* last = *head_ref;
//     while (last->next != NULL) {
//         last = last->next;
//     }

//     last->next = new;
// }

// void print_list(CNN_Data_Node* node) {
//     while (node != NULL) {
//         xil_printf("%d -> ", node->config_data.index);
//         node = node->next;
//     }
//     xil_printf("NULL\n");
// }

// int LAYER_CNN_load_data(CNN_Layer *instance, CNN_Config_Data data){

//     data.state = CONFIG_DATA_STATE_NOT_STARTED;

//     if(instance->data == NULL){
//         instance->data = create_data_node(data);
//     }
//     else{
//         append_node(&(instance->data), data);
//     }

//     instance->total_count++;

//     return 0;
// }


// static int LAYER_activate_softmax(Channel *channel){
//     int size = channel->total_bytes;
//     float* out_ptr = (float*)channel->output_ptr;

//     float max_val = out_ptr[0];
//     for (int i = 1; i < size; i++) {
//         if (out_ptr[i] > max_val) {
//             max_val = out_ptr[i];
//         }
//     }
//     // printf("max value %f\n", max_val);

//     float sum_exp = 0.0f;
//     for (int i = 0; i < size; i++) {
//         out_ptr[i] = expf(out_ptr[i] - max_val);
//         sum_exp += out_ptr[i];
//     }

//     // Normalize the softmax values
//     for (int i = 0; i < size; i++) {
//         out_ptr[i] /= sum_exp;
//     }
//     return 0;
// }

static int LAYER_activate_softmax(Layer *layer){

    Channel_Node* first_chan  = (Channel_Node*)layer->output_channels.channels;
    Channel_Node* second_chan = (Channel_Node*)first_chan->next;

    int height = first_chan->data.height;
    int width  = first_chan->data.width;

    float x1, x2, exp_x1, exp_x2, sum_exp, max_val;

    float *f_data_ptr, *s_data_ptr;

    f_data_ptr = (float*)first_chan->data.output_ptr;
    s_data_ptr = (float*)second_chan->data.output_ptr;

    // Iterate over each position in the spatial dimensions
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            // Get the input values for the current position
            x1 = f_data_ptr[(i*height)+j];
            x2 = s_data_ptr[(i*height)+j];

            // Compute max value for numerical stability
            max_val = fmaxf(x1, x2);

            // Compute exponentials and sum
            exp_x1  = expf(x1 - max_val);
            exp_x2  = expf(x2 - max_val);
            sum_exp = exp_x1 + exp_x2;

            // Normalize and store back
            f_data_ptr[(i*height)+j] = exp_x1 / sum_exp;
            s_data_ptr[(i*height)+j] = exp_x2 / sum_exp;
        }
    }
}

// void softmax(float *channel, int height, int width, int channels) {
//     int size = height * width * channels;

//     // Iterate over each pixel
//     for (int h = 0; h < height; ++h) {
//         for (int w = 0; w < width; ++w) {
//             // Compute the offset for the current pixel
//             int offset = (h * width + w) * channels;

//             // Find max value for numerical stability
//             float max = tensor[offset];
//             for (int c = 1; c < channels; ++c) {
//                 if (tensor[offset + c] > max) {
//                     max = tensor[offset + c];
//                 }
//             }

//             // Compute exponentials and sum
//             float sum = 0.0;
//             float exps[channels];
//             for (int c = 0; c < channels; ++c) {
//                 exps[c] = exp(tensor[offset + c] - max);
//                 sum += exps[c];
//             }

//             // Normalize and apply softmax
//             for (int c = 0; c < channels; ++c) {
//                 tensor[offset + c] = exps[c] / sum;
//             }
//         }
//     }
// }

static int LAYER_CNN_1x1_process(Layer *instance){
    int ret = 0;
    int input_position;
    float weight = 0.0f;
    Channel_Node* output_channel = instance->output_channels.channels;
    Channel_Node* input_channel;
    CNN_1x1_Data* data_ptr = NULL;


    float* output_ptr_f = NULL;
    float* input_ptr_f  = NULL;

    // xil_printf("CNN_1x1 process %d\r\n", instance->index);

    while(output_channel != NULL){
        input_position = 0;
        data_ptr = output_channel->data.cnn_1x1_data.data;
        // printf("Output Channel %d B(%f)\r\n", output_channel->data.index, *(float*)&data_ptr->bias);
        // xil_printf("\tChannel Process : I(%d) T(%d) H(%d) W(%d) OP(%p) TB(%d) MA(%d), MU(%d) \n", 
        //     output_channel->data.index, 
        //     output_channel->data.type, 
        //     output_channel->data.height,
        //     output_channel->data.width,
        //     output_channel->data.output_ptr,
        //     output_channel->data.total_bytes
        //     );

        input_channel = instance->input_channels.channels;

        output_ptr_f = (float*)output_channel->data.output_ptr;

        for (int i = 0; i < output_channel->data.total_bytes; i++) {
            output_ptr_f[i] = 0.0f;
        }

        while (input_channel != NULL) {
            weight = *(float*)&data_ptr->kernal_data[input_position];

            input_ptr_f = (float*)input_channel->data.input_ptr;
            // printf("\tInput Channel %d - W(%f) (%f) \n ", input_channel->data.index, weight, data_ptr->bias);

            for (int i = 0; i < output_channel->data.total_bytes; i++) {
                output_ptr_f[i] += (input_ptr_f[i] * weight);
            }
            // printf("\t\tPixel- O(%f) I(%f) W(%f)\n ",output_ptr_f[0], input_ptr_f[0], weight);
            // printf("1x1 data KC(%d), B(%f), D(%f) OD(%f)", data_ptr->kernal_count, data_ptr->bias, *(float*)&data_ptr->kernal_data[0], *(float*)&output_channel->data.output_ptr[0]);
            input_position++;
            input_channel = (Channel_Node*)input_channel->next;
        }

        for (int i = 0; i < output_channel->data.total_bytes; i++) {
            output_ptr_f[i] += data_ptr->bias;
        }


        output_channel = (Channel_Node*)output_channel->next;
    }

    if(instance->activation == LAYER_ACTIVATION_SOFTMAX){
        LAYER_activate_softmax(instance);
    }

    return ret;
}

static int LAYER_CNN_3x3_process(Layer *instance, Net_Engine_Inst *net_engine){
    int ret = 0;

    Channel_Node* cur_channel = instance->output_channels.channels;

    // check whether the channel loaded
    if(cur_channel == NULL){
        xil_printf("No output channel available \r\n");
        return 0;
    }

    // printf("Layer process init %d \r\n", instance->index);

    while (cur_channel != NULL){
        // xil_printf("\tChannel Process : I(%d) T(%d) H(%d) W(%d) OP(%p) TB(%d) MA(%d), MU(%d) \n", 
        //     cur_channel->data.index, 
        //     cur_channel->data.type, 
        //     cur_channel->data.height,
        //     cur_channel->data.width,
        //     cur_channel->data.output_ptr,
        //     cur_channel->data.total_bytes
        //     );
        // calling preprocess function
        if(instance->func.pre_process != NULL){
            instance->func.pre_process(*instance);
        }

        cur_channel->data.state = CHANNEL_STATE_BUSY;

#ifdef PROCESS_TIME_MEASURE
    measure_start(TIME_MEASURE_SIGNAL_2);
#endif

        CHANNEL_CNN_process(&cur_channel->data, net_engine);

#ifdef PROCESS_TIME_MEASURE
    measure_end(TIME_MEASURE_SIGNAL_2);
#endif
        cur_channel->data.state = CHANNEL_STATE_COMPLETED;

        if(instance->func.post_process != NULL){
            instance->func.post_process(*instance);
        }
        // jumping to next channel
        cur_channel = cur_channel->next;
    }

    // xil_printf("CNN Process Completed \r\n");

    return ret;
}


int LAYER_process(Layer *instance, void *optional){
    int ret = 0;

    xil_printf("Layer Process : I(%d) T(%d) H(%d) W(%d) MP(%p) MT(%p) MA(%d), MU(%d) \n", 
        instance->index, 
        instance->type, 
        instance->output_channels.channels->data.height,
        instance->output_channels.channels->data.width,
        instance->memory.memory_ptr,
        instance->memory.memory_tail,
        instance->memory.availale_mem_size,
        instance->memory.used_mem_size
        );

    instance->state = LAYER_STATE_BUSY;

    switch(instance->type){
        case LAYER_TYPE_CNN_1X1:          ret = LAYER_CNN_1x1_process(instance);                                break;
        case LAYER_TYPE_CNN_3X3:          ret = LAYER_CNN_3x3_process(instance, (Net_Engine_Inst*)optional);    break;
        case LAYER_TYPE_MAXPOOLING:       ret = LAYER_MAXPOOLING_process(instance);                             break;
        case LAYER_TYPE_CNN_2X2:
            xil_printf("Not Implement %d \r\n", instance->index);
            break;
    }

    instance->state = LAYER_STATE_COMPLETED;

    return ret;
}

    
int LAYER_update(Layer *input_layer, Layer *prev_layer, int height_, int width_){
    int ret       = 0;
    int prev_size = 0;

    int input_height  = 0;
    int input_width   = 0;
    int output_height  = 0;
    int output_width   = 0;
    int height         = 0;
    int width          = 0;

    Channel_Node* chan_node;

    if (prev_layer->index == input_layer->index){
        height = height_;
        width  = width_;
    }
    else{
        chan_node = prev_layer->output_channels.channels;
        if(chan_node != NULL){
            height = chan_node->data.height;
            width  = chan_node->data.width;
        }
        else{
            height = height_;
            width  = width_;
        }
    }

    switch(input_layer->type){
        case LAYER_TYPE_CNN_1X1:
        case LAYER_TYPE_CNN_2X2:
            input_height  = height;
            input_width   = width;
            output_height = height;
            output_width  = width;
            break;
        case LAYER_TYPE_CNN_3X3:
            input_height  = height;
            input_width   = width;
            output_height = height - 2;
            output_width  = output_height;
            break;
        case LAYER_TYPE_MAXPOOLING:
            input_height  = height;
            input_width   = width;
            output_height = (((height-2) + 2)/2);
            output_width  = output_height;
            break;
    }
    
    // printf("input_height(%d), input_width(%d), output_height(%d), output_width(%d) \n", input_height, input_width, output_height, output_width);

    Channel_Node* input_channel  = input_layer->input_channels.channels;
    Channel_Node* output_channel = input_layer->output_channels.channels;

    // check whether the channel loaded
    if(input_channel == NULL || output_channel == NULL){
        xil_printf("No output channel available \r\n");
        return 0;
    }

    while (input_channel != NULL){
        CHANNEL_update((&input_channel->data), input_height, input_width);
        
        input_channel  = input_channel->next;
    }  

    while (output_channel != NULL){

        CHANNEL_update(&output_channel->data, output_height, output_width);

        // jumping to next channel
        output_channel = output_channel->next;
    }  
}