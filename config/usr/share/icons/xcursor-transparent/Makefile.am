CURSOR_NAMES = \
	00008160000006810000408080010102 \
	028006030e0e7ebffc7f7070c0600140 \
	03b6e0fcb3499374a867c041f52298f0 \
	08e8e1c95fe2fc01f976f1e063a24ccd \
	14fef782d02440884392942c11205230 \
	2870a09082c103050810ffdffffe0204 \
	3ecb610c1bf2410f44200f48c40d3599 \
	4498f0e0c1937ffe01fd06f973665830 \
	9d800788f1b08800ae810202380a0822 \
	c7088f0f3e6c8088236ef8e1e3e70000 \
	d9ce0ab605698f320427677b458ad60b \
	e29285e634086352946a0e7090d73106 \
	fcf1c3c7cd4491d801f1e1c78f100000 \
	X_cursor \
	arrow \
	base_arrow_down \
	base_arrow_up \
	based_arrow_down \
	based_arrow_up \
	bd_double_arrow \
	boat \
	bottom_left_corner \
	bottom_right_corner \
	bottom_side \
	bottom_tee \
	center_ptr \
	circle \
	closedhand \
	color-picker \
	copy \
	cross \
	cross_reverse \
	crossed_circle \
	crosshair \
	dnd-copy \
	dnd-move \
	dnd-no-drop \
	dnd-none \
	dot \
	dot_box_mask \
	dotbox \
	double_arrow \
	draft_large  \
	draft_small  \
	draped_box   \
	exchange     \
	fd_double_arrow \
	fleur \
	forbidden \
	gumby \
	h_double_arrow \
	hand \
	hand1 \
	hand2 \
	ibeam \
	left_ptr \
	left_ptr_watch \
	left_side \
	left_tee  \
	link \
	ll_angle  \
	lr_angle  \
	move      \
	openhand \
	pencil    \
	pirate    \
	plus      \
	pointing_hand \
	progress \
	question_arrow \
	right_ptr      \
	right_side     \
	right_tee      \
	sailboat       \
	sb_down_arrow  \
	sb_h_double_arrow \
	sb_left_arrow \
	sb_right_arrow \
	sb_up_arrow \
	sb_v_double_arrow \
	shuttle \
	size_all \
	size_bdiag \
	size_fdiag \
	size_hor \
	size_ver \
	sizing  \
	split_h \
	split_v \
	target  \
	tcross  \
	text \
	top_left_arrow  \
	top_left_corner \
	top_right_corner \
	top_side \
	top_tee \
	trek \
	ul_angle \
	up_arrow \
	ur_angle \
	v_double_arrow \
	wait \
	watch \
	whats_this \
	xterm

CURSOR_DIR = $(datadir)/icons/xcursor-transparent/cursors
CURSOR_REAL = transp
EXTRA_DIST = $(CURSOR_REAL)

install-data-local:
	$(mkinstalldirs) $(DESTDIR)$(CURSOR_DIR);
	$(INSTALL_DATA) $(srcdir)/$(CURSOR_REAL) $(DESTDIR)$(CURSOR_DIR)/
	for CURSOR in $(CURSOR_NAMES); do \
		echo '-- Installing cursor '$$CURSOR; \
		ln -s transp $(DESTDIR)$(CURSOR_DIR)/$$CURSOR; \
	done

