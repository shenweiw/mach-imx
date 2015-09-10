# mach-imx

This repo is used to record the development of GPCv2 driver for Linux-next. Recently the upstreaming linux does not seem to accept the platform dependent codes for power management like suspend and standby. They perfer to implement this kind of feature via the new PSCI framework. Under PSCI framework, the platform dependent codes will be moved into the platform firmare which is running under Hypervisor or Secure priviledge. The support for this firmware is not readby on I.MX6/7 platforms. Before the PSCI based solution is ready, this repo will keep update this platform dependent implementation, and will keep tracking the changes on linux-next and make the power management codes always align and consistent with the upsteaming kernel.

# GPCv2 Introduction

IMX7D contains a new version of GPC IP block (GPCv2). It has two major functions: power management and wakeup source management.

Two drivers were developed to support these functions: 
One irqchip driver (irq-imx-gpcv2.c) is to manage the interrupt wakeup source. 
One suspend driver (pm-imx7.c) is used to manage the system power states.
Currently the irqchip driver has been accepted by linux-next, so it is not maintained here.

The suspend driver provides low power mode control for Cortex-A7 and Cortex-M4 domains. And it can support WAIT, STOP, and DSM(Deep Sleep Mode) modes. After configuring the GPCv2 module, the platform can enter into a selected mode either automatically triggered by ARM WFI instruction or manually by software. The system will exit the low power states by the predefined wakeup sources which are managed by the gpcv2 irqchip driver.

# Test

Select a wakeup source. The following is an example to select the serial port 0 as the only wakeup source. Of course, you can select multiple sources one time.

    echo enabled > /sys/class/tty/ttymxc0/power/wakeup

Let the system go into suspend state:

    echo mem > /sys/power/state

The system should be in suspend state now. You can verify this by monitoring the power supply. To wakeup the system, just click any key in the serial console.

    

