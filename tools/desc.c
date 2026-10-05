#include <stdio.h>
#include <libusb-1.0/libusb.h>

static const char *xfer(int a){switch(a&3){case 0:return "CONTROL";case 1:return "ISOCHRONOUS";case 2:return "BULK";default:return "INTERRUPT";}}

int main(void){
  libusb_context *ctx=NULL;
  if(libusb_init(&ctx)<0){puts("init failed");return 1;}
  libusb_device **list; ssize_t n=libusb_get_device_list(ctx,&list);
  for(ssize_t i=0;i<n;i++){
    struct libusb_device_descriptor dd;
    if(libusb_get_device_descriptor(list[i],&dd)<0) continue;
    if(!(dd.idVendor==0x07ca && dd.idProduct==0xc835)) continue;
    printf("DEVICE %04x:%04x  bcdUSB=%04x class=%u/%u/%u numCfg=%u\n",
      dd.idVendor,dd.idProduct,dd.bcdUSB,dd.bDeviceClass,dd.bDeviceSubClass,dd.bDeviceProtocol,dd.bNumConfigurations);
    for(int c=0;c<dd.bNumConfigurations;c++){
      struct libusb_config_descriptor *cfg;
      if(libusb_get_config_descriptor(list[i],c,&cfg)<0){printf("  cfg %d: unreadable\n",c);continue;}
      printf("  CONFIG %u: numIfaces=%u totalLen=%u maxPower=%umA attrs=0x%02x\n",
        cfg->bConfigurationValue,cfg->bNumInterfaces,cfg->wTotalLength,cfg->MaxPower*2,cfg->bmAttributes);
      for(int ii=0;ii<cfg->bNumInterfaces;ii++){
        const struct libusb_interface *itf=&cfg->interface[ii];
        printf("   INTERFACE %d: %d altsetting(s)\n",ii,itf->num_altsetting);
        for(int a=0;a<itf->num_altsetting;a++){
          const struct libusb_interface_descriptor *id=&itf->altsetting[a];
          printf("    alt=%u ifnum=%u class=%u sub=%u proto=%u numEP=%u\n",
            id->bAlternateSetting,id->bInterfaceNumber,id->bInterfaceClass,id->bInterfaceSubClass,id->bInterfaceProtocol,id->bNumEndpoints);
          if(id->extra_length>0){
            printf("      extra(%d):",id->extra_length);
            for(int k=0;k<id->extra_length;k++)printf(" %02x",id->extra[k]);
            printf("\n");
          }
          for(int e=0;e<id->bNumEndpoints;e++){
            const struct libusb_endpoint_descriptor *ep=&id->endpoint[e];
            printf("      EP 0x%02x %-4s %-11s wMaxPacket=%u bInterval=%u\n",
              ep->bEndpointAddress,(ep->bEndpointAddress&0x80)?"IN":"OUT",
              xfer(ep->bmAttributes),ep->wMaxPacketSize,ep->bInterval);
          }
        }
      }
      libusb_free_config_descriptor(cfg);
    }
  }
  libusb_free_device_list(list,1); libusb_exit(ctx); return 0;
}
