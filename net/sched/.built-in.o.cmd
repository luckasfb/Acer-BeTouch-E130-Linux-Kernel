cmd_net/sched/built-in.o :=  /opt/arm-2008q1/bin/arm-none-linux-gnueabi-ld -EL    -r -o net/sched/built-in.o net/sched/sch_generic.o net/sched/sch_api.o net/sched/sch_blackhole.o net/sched/cls_api.o net/sched/act_api.o net/sched/act_police.o net/sched/sch_fifo.o net/sched/sch_ingress.o net/sched/cls_u32.o 