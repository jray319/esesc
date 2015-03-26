#ifndef MSG_ACTION_H
#define MSG_ACTION_H

// MsgAction enumerate {{{1
//
// MOESI States:
//
// M: Single copy, memory not consistent
//
// E: Single copy, memory is consistent
//
// I: invalid
//
// S: one of (potentially) several copies. Share does not respond to other bus snoop reads
// 
// O: Like shared, but the O is responsible to update memory. If O does
// a write back, it can change to S
//
// [sizhuo] setInvalid, setShared -- downgrade req
// setValid, setExclusive, setDirty -- upgrade req
enum MsgAction {
	ma_setInvalid,
	ma_setValid,
	ma_setDirty,
	ma_setShared,
	ma_setExclusive,
	ma_MMU,
	ma_VPCWU,
	ma_MAX
};

// }}}
#endif
