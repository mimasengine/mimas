# SRL pre-build hook (auto-included by shared.mk; chain: build -> pre_build).
# Generates the project IP.BIN boot header consumed via SRL_IPBIN (Makefile):
# maker N0rt0N85, product Mimas-Eng (10-char field, SAROO config key),
# title "Mimas Engine", version V0.<git commit count>, release date = build
# date.  See tools/make_ip.py.
PYTHON ?= python

pre_build:
	@mkdir -p $(BUILD_DROP)
	$(PYTHON) tools/make_ip.py \
		--template $(SRL_INSTALL_ROOT)/modules/sgl/IP.BIN \
		--out $(BUILD_DROP)/IP.BIN \
		--maker "N0rt0N85" \
		--product "Mimas-Eng" \
		--title "Mimas Engine" \
		--build-num git

# The ISO step consumes the generated IP.BIN -- make the dependency explicit
# so the ordering also holds under parallel make, not just via the
# 'build: pre_build build_bin_cue' left-to-right prerequisite order.
create_iso: pre_build
