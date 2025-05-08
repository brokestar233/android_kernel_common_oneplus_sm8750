#ifndef _ZRAM_DRV_INTERNAL_H_
#define _ZRAM_DRV_INTERNAL_H_
#ifdef BIT
#undef BIT
#define BIT(nr)		(1lu << (nr))
#endif

enum {
	ZRAM_TYPE_BASEPAGE,
	ZRAM_TYPE_MAX,
};

enum {
	ZRAM_STATE_TOTAL,
	ZRAM_STATE_USED,
	ZRAM_STATE_SAME_PAGE,
	ZRAM_STATE_COMPRESSED_PAGE,
};

extern struct zram *zram_arr[ZRAM_TYPE_MAX];

#define zram_slot_lock(zram, index) (bit_spin_lock(ZRAM_LOCK, &zram->table[index].flags))

#define zram_slot_unlock(zram, index) (bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags))

#define init_done(zram)  (zram->disksize)

#define dev_to_zram(dev) ((struct zram *)(dev_to_disk(dev)->private_data))

#define zram_get_handle(zram, index) (zram->table[index].handle)

#define zram_set_handle(zram, index, handle_val) (zram->table[index].handle = handle_val)

#define zram_test_flag(zram, index,  flag) (zram->table[index].flags & BIT(flag))

#define zram_set_flag(zram, index, flag) (zram->table[index].flags |= BIT(flag))

#define zram_clear_flag(zram, index, flag) (zram->table[index].flags &= ~BIT(flag))

#define zram_set_element(zram, index, element) (zram->table[index].element = element)

#define zram_get_obj_size(zram, index) (zram->table[index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1))

#define zram_set_obj_size(zram, index, size) do {\
	unsigned long flags = zram->table[index].flags >> ZRAM_FLAG_SHIFT; \
	zram->table[index].flags = (flags << ZRAM_FLAG_SHIFT) | size; \
} while(0)

extern bool chp_supported;

extern inline bool is_chp_zram(struct zram *zram);
extern inline unsigned long zram_page_state(struct zram *zram, int type);
#endif
