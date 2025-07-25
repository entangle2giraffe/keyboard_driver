// usbkbd_cmdmode.c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/input.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI ChatGPT");
MODULE_DESCRIPTION("Simple USB Keyboard Command Mode Driver (Ctrl+Space toggle, b=open terminal, q=quit)");

#define USB_HID_CLASS 3
#define USB_HID_SUBCLASS 1
#define USB_HID_PROTOCOL_KEYBOARD 1

struct usbkbd_cmd
{
    struct usb_device *usbdev;
    struct usb_interface *interface;

    struct input_dev *virt_input;
    struct urb *irq;
    unsigned char *data;
    dma_addr_t data_dma;
    int data_size;

    bool command_mode;
    bool ctrl_pressed;
    bool space_pressed;
    bool key_state[KEY_MAX];
};

static void send_key(struct usbkbd_cmd *kbd, unsigned int keycode, bool pressed)
{
    input_report_key(kbd->virt_input, keycode, pressed);
    input_sync(kbd->virt_input);
}

static void send_ctrl_alt_t(struct usbkbd_cmd *kbd)
{
    send_key(kbd, KEY_LEFTCTRL, true);
    send_key(kbd, KEY_LEFTALT, true);
    send_key(kbd, KEY_T, true);
    send_key(kbd, KEY_T, false);
    send_key(kbd, KEY_LEFTALT, false);
    send_key(kbd, KEY_LEFTCTRL, false);
}

static void process_key(struct usbkbd_cmd *kbd, unsigned char keycode, bool pressed)
{
    if (keycode >= KEY_MAX)
        return;

    kbd->key_state[keycode] = pressed;

    if (keycode == KEY_LEFTCTRL || keycode == KEY_RIGHTCTRL)
        kbd->ctrl_pressed = pressed;

    if (keycode == KEY_SPACE)
        kbd->space_pressed = pressed;

    // Detect Ctrl + Space toggle (on key press)
    if (kbd->ctrl_pressed && kbd->space_pressed && pressed)
    {
        kbd->command_mode = !kbd->command_mode;
        printk(KERN_INFO "usbkbd_cmdmode: Command Mode %s\n", kbd->command_mode ? "ENABLED" : "DISABLED");
        return;
    }

    if (!kbd->command_mode)
    {
        // Normal mode: pass keys through unchanged
        send_key(kbd, keycode, pressed);
        return;
    }

    if (!pressed)
        return;

    switch (keycode)
    {
    case KEY_B:
        send_ctrl_alt_t(kbd);
        // Release 'b' key after handling
        send_key(kbd, KEY_B, false);
        break;
    case KEY_Q:
        kbd->command_mode = false;
        printk(KERN_INFO "usbkbd_cmdmode: Exiting Command Mode\n");
        // Release 'q' key after handling
        send_key(kbd, KEY_Q, false);
        break;
    default:
        // Ignore other keys in command mode, but release them if pressed
        send_key(kbd, keycode, false);
        break;
    }
}

static void usbkbd_cmd_irq(struct urb *urb)
{
    struct usbkbd_cmd *kbd = urb->context;
    int i;

    if (urb->status)
    {
        printk(KERN_ERR "usbkbd_cmdmode: URB status %d\n", urb->status);
        return;
    }

    unsigned char *data = kbd->data;
    unsigned char keys[6];
    memcpy(keys, &data[2], 6);

    static unsigned char old_keys[6] = {0};

    // Release keys no longer pressed
    for (i = 0; i < 6; i++)
    {
        int j;
        bool found = false;
        if (old_keys[i] == 0)
            continue;
        for (j = 0; j < 6; j++)
        {
            if (old_keys[i] == keys[j])
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            process_key(kbd, old_keys[i], false);
        }
    }

    // Press new keys
    for (i = 0; i < 6; i++)
    {
        int j;
        bool found = false;
        if (keys[i] == 0)
            continue;
        for (j = 0; j < 6; j++)
        {
            if (old_keys[j] == keys[i])
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            process_key(kbd, keys[i], true);
        }
    }

    memcpy(old_keys, keys, 6);

    usb_submit_urb(kbd->irq, GFP_ATOMIC);
}

static int usbkbd_cmd_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    struct usbkbd_cmd *kbd;
    struct usb_device *dev = interface_to_usbdev(interface);
    struct usb_endpoint_descriptor *endpoint;
    int pipe, retval, i;

    kbd = kzalloc(sizeof(*kbd), GFP_KERNEL);
    if (!kbd)
        return -ENOMEM;

    kbd->usbdev = dev;
    kbd->interface = interface;
    kbd->command_mode = false;

    kbd->virt_input = input_allocate_device();
    if (!kbd->virt_input)
    {
        kfree(kbd);
        return -ENOMEM;
    }

    kbd->virt_input->name = "usbkbd_cmdmode_virtual";
    kbd->virt_input->phys = "usb/input0";
    kbd->virt_input->id.bustype = BUS_USB;
    kbd->virt_input->id.vendor = le16_to_cpu(dev->descriptor.idVendor);
    kbd->virt_input->id.product = le16_to_cpu(dev->descriptor.idProduct);
    kbd->virt_input->id.version = le16_to_cpu(dev->descriptor.bcdDevice);

    __set_bit(EV_KEY, kbd->virt_input->evbit);
    for (i = 0; i < KEY_MAX; i++)
        __set_bit(i, kbd->virt_input->keybit);

    retval = input_register_device(kbd->virt_input);
    if (retval)
    {
        input_free_device(kbd->virt_input);
        kfree(kbd);
        return retval;
    }

    endpoint = NULL;
    for (i = 0; i < interface->cur_altsetting->desc.bNumEndpoints; i++)
    {
        struct usb_endpoint_descriptor *ep = &interface->cur_altsetting->endpoint[i].desc;
        if (usb_endpoint_is_int_in(ep))
        {
            endpoint = ep;
            break;
        }
    }

    if (!endpoint)
    {
        printk(KERN_ERR "usbkbd_cmdmode: no interrupt endpoint\n");
        input_unregister_device(kbd->virt_input);
        kfree(kbd);
        return -ENODEV;
    }

    kbd->data_size = endpoint->wMaxPacketSize;
    kbd->data = usb_alloc_coherent(dev, kbd->data_size, GFP_KERNEL, &kbd->data_dma);
    if (!kbd->data)
    {
        input_unregister_device(kbd->virt_input);
        kfree(kbd);
        return -ENOMEM;
    }

    pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);
    kbd->irq = usb_alloc_urb(0, GFP_KERNEL);
    if (!kbd->irq)
    {
        usb_free_coherent(dev, kbd->data_size, kbd->data, kbd->data_dma);
        input_unregister_device(kbd->virt_input);
        kfree(kbd);
        return -ENOMEM;
    }

    usb_fill_int_urb(kbd->irq, dev, pipe, kbd->data, kbd->data_size,
                     usbkbd_cmd_irq, kbd, endpoint->bInterval);
    kbd->irq->transfer_dma = kbd->data_dma;
    kbd->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    usb_set_intfdata(interface, kbd);

    retval = usb_submit_urb(kbd->irq, GFP_KERNEL);
    if (retval)
    {
        usb_free_urb(kbd->irq);
        usb_free_coherent(dev, kbd->data_size, kbd->data, kbd->data_dma);
        input_unregister_device(kbd->virt_input);
        kfree(kbd);
        return retval;
    }

    printk(KERN_INFO "usbkbd_cmdmode: USB keyboard registered\n");
    return 0;
}

static void usbkbd_cmd_disconnect(struct usb_interface *interface)
{
    struct usbkbd_cmd *kbd = usb_get_intfdata(interface);
    if (!kbd)
        return;

    usb_kill_urb(kbd->irq);
    usb_free_urb(kbd->irq);
    usb_free_coherent(kbd->usbdev, kbd->data_size, kbd->data, kbd->data_dma);
    input_unregister_device(kbd->virt_input);
    kfree(kbd);

    printk(KERN_INFO "usbkbd_cmdmode: USB keyboard disconnected\n");
}

static const struct usb_device_id usbkbd_cmd_table[] = {
    {USB_INTERFACE_INFO(USB_HID_CLASS, USB_HID_SUBCLASS, USB_HID_PROTOCOL_KEYBOARD)},
    {}};
MODULE_DEVICE_TABLE(usb, usbkbd_cmd_table);

static struct usb_driver usbkbd_cmd_driver = {
    .name = "usbkbd_cmdmode",
    .probe = usbkbd_cmd_probe,
    .disconnect = usbkbd_cmd_disconnect,
    .id_table = usbkbd_cmd_table,
};

module_usb_driver(usbkbd_cmd_driver);
