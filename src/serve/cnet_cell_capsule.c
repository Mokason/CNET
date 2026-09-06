#include "cnet_cell_capsule.h"
int cnet_core_cell_to_btn(const CnetCoreCell *cell,Port input,Port output,BinaryTransformNetwork *btn){
    if(!btn||cnet_core_cell_validate(cell)||input.family!=PORT_BINARY_MSB||output.family!=PORT_BINARY_MSB||
       input.field_width!=3||output.field_width!=1||input.field_count!=1||output.field_count!=1)return -1;
    if(btn_init(btn,3,1,8,8,.1,7))return -1;
    if(btn_set_ports(btn,input,output)){btn_free(btn);return -1;}
    for(unsigned h=0;h<8;h++){
        for(unsigned i=0;i<3;i++)btn->input_hidden[h*3+i]=cell->weight[h*4+i];
        btn->hidden_bias[h]=cell->weight[h*4+3];btn->hidden_output_weights[h]=cell->weight[32+h];
    }
    btn->output_bias[0]=cell->weight[40];return 0;
}
