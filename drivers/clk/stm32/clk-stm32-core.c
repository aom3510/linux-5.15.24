// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STMicroelectronics 2022 - All Rights Reserved
 * Author: Gabriel Fernandez <gabriel.fernandez@foss.st.com> for STMicroelectronics.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "clk-stm32-core.h"
#include "reset-stm32.h"

static DEFINE_SPINLOCK(rlock);

static int stm32_rcc_clock_init(struct device *dev,
				const struct of_device_id *match,
				void __iomem *base)
{
	const struct stm32_rcc_match_data *data = match->data;
	struct clk_hw_onecell_data *clk_data = data->hw_clks;
	struct clk_hw **hws;
	int n, max_binding;

	max_binding =  data->maxbinding;

	clk_data = devm_kzalloc(dev, struct_size(clk_data, hws, max_binding),
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->num = max_binding;

	hws = clk_data->hws;

	for (n = 0; n < max_binding; n++)
		hws[n] = ERR_PTR(-ENOENT);

	for (n = 0; n < data->num_clocks; n++) {
		const struct clock_config *cfg_clock = &data->tab_clocks[n];
		struct clk_hw *hw = ERR_PTR(-ENOENT);

		if (data->check_security &&
		    data->check_security(base, cfg_clock))
			continue;

		if (cfg_clock->func)
			hw = (*cfg_clock->func)(dev, data, base, &rlock,
						cfg_clock);

		if (IS_ERR(hw)) {
			dev_err(dev, "Can't register clk %d: %ld\n", n,
				PTR_ERR(hw));
			return PTR_ERR(hw);
		}

		if (cfg_clock->id < NO_ID)
			hws[cfg_clock->id] = hw;
	}

	return of_clk_add_hw_provider(dev_of_node(dev), of_clk_hw_onecell_get,
				      clk_data);
}

int stm32_rcc_init(struct device *dev, const struct of_device_id *match_data,
		   void __iomem *base)
{
	const struct of_device_id *match;
	int err;

	match = of_match_node(match_data, dev_of_node(dev));
	if (!match) {
		dev_err(dev, "match data not found\n");
		return -ENODEV;
	}

	/* RCC Reset Configuration */
	err = stm32_rcc_reset_init(dev, match, base);
	if (err) {
		pr_err("stm32mp1 reset failed to initialize\n");
		return err;
	}

	/* RCC Clock Configuration */
	err = stm32_rcc_clock_init(dev, match, base);
	if (err) {
		pr_err("stm32mp1 clock failed to initialize\n");
		return err;
	}

	return 0;
}

void clk_stm32_endisable_gate(void __iomem *base,
			      struct clk_stm32_clock_data *data,
			      u16 gate_id, int enable)
{
	const struct stm32_gate_cfg *gate = &data->gates[gate_id];
	void __iomem *addr = base + gate->offset;

	if (enable) {
		if (data->gate_cpt[gate_id]++ > 0)
			return;

		if (gate->set_clr != 0)
			writel(BIT(gate->bit_idx), addr);
		else
			writel(readl(addr) | BIT(gate->bit_idx), addr);
	} else {
		if (--data->gate_cpt[gate_id] > 0)
			return;

		if (gate->set_clr != 0)
			writel(BIT(gate->bit_idx), addr + gate->set_clr);
		else
			writel(readl(addr) & ~BIT(gate->bit_idx), addr);
	}
}

static void clk_stm32_disable_unused_gate(void __iomem *base,
					  struct clk_stm32_clock_data *data,
					  u16 gate_id)
{
	const struct stm32_gate_cfg *gate = &data->gates[gate_id];
	void __iomem *addr = base + gate->offset;

	if (data->gate_cpt[gate_id] > 0)
		return;

	if (gate->set_clr != 0)
		writel(BIT(gate->bit_idx), addr + gate->set_clr);
	else
		writel(readl(addr) & ~BIT(gate->bit_idx), addr);
}

int clk_stm32_is_enabled_gate(void __iomem *base,
			      struct clk_stm32_clock_data *data,
			      u16 gate_id)
{
	const struct stm32_gate_cfg *gate = &data->gates[gate_id];

	return (readl(base + gate->offset) & BIT(gate->bit_idx)) != 0;
}

void clk_stm32_gate_endisable(struct clk_hw *hw, int enable)
{
	struct clk_stm32_gate *gate = to_clk_stm32_gate(hw);
	unsigned long flags = 0;

	spin_lock_irqsave(gate->lock, flags);

	clk_stm32_endisable_gate(gate->base, gate->clock_data,
				 gate->gate_id, enable);

	spin_unlock_irqrestore(gate->lock, flags);
}

int clk_stm32_gate_enable(struct clk_hw *hw)
{
	clk_stm32_gate_endisable(hw, 1);

	return 0;
}

void clk_stm32_gate_disable(struct clk_hw *hw)
{
	clk_stm32_gate_endisable(hw, 0);
}

int clk_stm32_gate_is_enabled(struct clk_hw *hw)
{
	struct clk_stm32_gate *gate = to_clk_stm32_gate(hw);

	return clk_stm32_is_enabled_gate(gate->base, gate->clock_data,
					 gate->gate_id);
}

static void clk_stm32_gate_disable_unused(struct clk_hw *hw)
{
	struct clk_stm32_gate *gate = to_clk_stm32_gate(hw);
	unsigned long flags = 0;

	spin_lock_irqsave(gate->lock, flags);

	clk_stm32_disable_unused_gate(gate->base, gate->clock_data, gate->gate_id);

	spin_unlock_irqrestore(gate->lock, flags);
}

const struct clk_ops clk_stm32_gate_ops = {
	.enable		= clk_stm32_gate_enable,
	.disable	= clk_stm32_gate_disable,
	.is_enabled	= clk_stm32_gate_is_enabled,
	.disable_unused	= clk_stm32_gate_disable_unused,
};

#define MUX_SAFE_POSITION 0

static int clk_stm32_has_safe_mux(struct clk_hw *hw)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);
	const struct stm32_mux_cfg *mux = &composite->clock_data->muxes[composite->mux_id];

	return !!(mux->flags & MUX_SAFE);
}

static void clk_stm32_set_safe_position_mux(struct clk_hw *hw)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);

	if (!clk_stm32_composite_is_enabled(hw)) {
		unsigned long flags = 0;

		if (composite->clock_data->is_multi_mux) {
			struct clk_hw *other_mux_hw = NULL;

			other_mux_hw = composite->clock_data->is_multi_mux(hw);

			if (!other_mux_hw || clk_stm32_composite_is_enabled(other_mux_hw))
				return;
		}

		spin_lock_irqsave(composite->lock, flags);

		clk_stm32_set_parent_mux(composite->base, composite->clock_data,
					 composite->mux_id, MUX_SAFE_POSITION);

		spin_unlock_irqrestore(composite->lock, flags);
	}
}

static void clk_stm32_safe_restore_position_mux(struct clk_hw *hw)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);
	int sel = clk_hw_get_parent_index(hw);
	unsigned long flags = 0;

	spin_lock_irqsave(composite->lock, flags);

	clk_stm32_set_parent_mux(composite->base, composite->clock_data,
				 composite->mux_id, sel);

	spin_unlock_irqrestore(composite->lock, flags);
}

void clk_stm32_composite_gate_endisable(struct clk_hw *hw, int enable)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);
	unsigned long flags = 0;

	spin_lock_irqsave(composite->lock, flags);

	clk_stm32_endisable_gate(composite->base, composite->clock_data,
				 composite->gate_id, enable);

	spin_unlock_irqrestore(composite->lock, flags);
}

int clk_stm32_composite_gate_enable(struct clk_hw *hw)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);

	if (composite->gate_id == NO_STM32_GATE)
		return 0;

	clk_stm32_composite_gate_endisable(hw, 1);

	if (composite->mux_id != NO_STM32_MUX && clk_stm32_has_safe_mux(hw))
		clk_stm32_safe_restore_position_mux(hw);

	return 0;
}

void clk_stm32_composite_gate_disable(struct clk_hw *hw)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);

	if (composite->gate_id == NO_STM32_GATE)
		return;

	clk_stm32_composite_gate_endisable(hw, 0);

	if (composite->mux_id != NO_STM32_MUX && clk_stm32_has_safe_mux(hw))
		clk_stm32_set_safe_position_mux(hw);
}

int clk_stm32_composite_is_enabled(struct clk_hw *hw)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);

	if (composite->gate_id == NO_STM32_GATE)
		return (__clk_get_enable_count(hw->clk) > 0);

	return clk_stm32_is_enabled_gate(composite->base, composite->clock_data,
					 composite->gate_id);
}

static void clk_stm32_composite_disable_unused(struct clk_hw *hw)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);
	unsigned long flags = 0;

	if (composite->gate_id == NO_STM32_GATE)
		return;

	spin_lock_irqsave(composite->lock, flags);

	clk_stm32_disable_unused_gate(composite->base, composite->clock_data,
				      composite->gate_id);

	spin_unlock_irqrestore(composite->lock, flags);
}

u8 clk_stm32_get_parent_mux(void __iomem *base,
			    struct clk_stm32_clock_data *data, u16 mux_id)
{
	const struct stm32_mux_cfg *mux = &data->muxes[mux_id];
	u32 mask = BIT(mux->width) - 1;
	u32 val;

	val = readl(base + mux->offset) >> mux->shift;
	val &= mask;

	return val;
}

int clk_stm32_set_parent_mux(void __iomem *base,
			     struct clk_stm32_clock_data *data,
			     u16 mux_id, u8 index)
{
	const struct stm32_mux_cfg *mux = &data->muxes[mux_id];

	u32 mask = BIT(mux->width) - 1;
	u32 reg = readl(base + mux->offset);
	u32 val = index << mux->shift;

	reg &= ~(mask << mux->shift);
	reg |= val;

	writel(reg, base + mux->offset);

	return 0;
}

u8 clk_stm32_composite_get_parent(struct clk_hw *hw)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);

	return clk_stm32_get_parent_mux(composite->base, composite->clock_data,
					composite->mux_id);
}

int clk_stm32_composite_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);
	unsigned long flags = 0;

	spin_lock_irqsave(composite->lock, flags);

	clk_stm32_set_parent_mux(composite->base, composite->clock_data,
				 composite->mux_id, index);

	spin_unlock_irqrestore(composite->lock, flags);

	if (composite->clock_data->is_multi_mux) {
		struct clk_hw *other_mux_hw = composite->clock_data->is_multi_mux(hw);

		if (other_mux_hw) {
			struct clk_hw *hwp = clk_hw_get_parent_by_index(hw, index);

			clk_hw_reparent(other_mux_hw, hwp);
		}
	}
	return 0;
}

static unsigned int _get_table_div(const struct clk_div_table *table,
				   unsigned int val)
{
	const struct clk_div_table *clkt;

	for (clkt = table; clkt->div; clkt++)
		if (clkt->val == val)
			return clkt->div;
	return 0;
}

static unsigned int _get_div(const struct clk_div_table *table,
			     unsigned int val, unsigned long flags, u8 width)
{
	if (flags & CLK_DIVIDER_ONE_BASED)
		return val;
	if (flags & CLK_DIVIDER_POWER_OF_TWO)
		return 1 << val;
	if (table)
		return _get_table_div(table, val);
	return val + 1;
}

unsigned long clk_stm32_get_rate_divider(void __iomem *base,
					 struct clk_stm32_clock_data *data,
					 u16 div_id, unsigned long parent_rate)
{
	const struct stm32_div_cfg *divider = &data->dividers[div_id];
	unsigned int val;
	unsigned int div;

	val =  readl(base + divider->offset) >> divider->shift;
	val &= clk_div_mask(divider->width);
	div = _get_div(divider->table, val, divider->flags, divider->width);

	if (!div) {
		WARN(!(divider->flags & CLK_DIVIDER_ALLOW_ZERO),
		     "%d: Zero divisor and CLK_DIVIDER_ALLOW_ZERO not set\n",
		     div_id);
		return parent_rate;
	}

	return DIV_ROUND_UP_ULL((u64)parent_rate, div);
}

unsigned long clk_stm32_composite_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);

	if (composite->div_id == NO_STM32_DIV)
		return parent_rate;

	return clk_stm32_get_rate_divider(composite->base,
					  composite->clock_data,
					  composite->div_id,
					  parent_rate);
}

int clk_stm32_set_rate_divider(void __iomem *base,
			       struct clk_stm32_clock_data *data,
			       u16 div_id,
			       unsigned long rate,
			       unsigned long parent_rate)
{
	const struct stm32_div_cfg *divider = &data->dividers[div_id];
	int value;
	u32 val;

	value = divider_get_val(rate, parent_rate, divider->table,
				divider->width, divider->flags);
	if (value < 0)
		return value;

	if (divider->flags & CLK_DIVIDER_HIWORD_MASK) {
		val = clk_div_mask(divider->width) << (divider->shift + 16);
	} else {
		val = readl(base + divider->offset);
		val &= ~(clk_div_mask(divider->width) << divider->shift);
	}

	val |= (u32)value << divider->shift;

	writel(val, base + divider->offset);

	return 0;
}

int clk_stm32_composite_set_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);
	unsigned long flags = 0;
	int ret;

	if (composite->div_id == NO_STM32_DIV)
		return rate;

	spin_lock_irqsave(composite->lock, flags);

	ret = clk_stm32_set_rate_divider(composite->base, composite->clock_data,
					 composite->div_id, rate, parent_rate);

	spin_unlock_irqrestore(composite->lock, flags);

	return ret;
}

long clk_stm32_composite_round_rate(struct clk_hw *hw, unsigned long rate,
				    unsigned long *prate)
{
	struct clk_stm32_composite *composite = to_clk_stm32_composite(hw);

	const struct stm32_div_cfg *divider;

	if (composite->div_id == NO_STM32_DIV)
		return rate;

	divider = &composite->clock_data->dividers[composite->div_id];

	/* if read only, just return current value */
	if (divider->flags & CLK_DIVIDER_READ_ONLY) {
		u32 val;

		val =  readl(composite->base + divider->offset) >> divider->shift;
		val &= clk_div_mask(divider->width);

		return divider_ro_round_rate(hw, rate, prate, divider->table,
				divider->width, divider->flags,
				val);
	}

	return divider_round_rate_parent(hw, clk_hw_get_parent(hw),
					 rate, prate, divider->table,
					 divider->width, divider->flags);
}

const struct clk_ops clk_stm32_composite_ops = {
	.set_rate	= clk_stm32_composite_set_rate,
	.recalc_rate	= clk_stm32_composite_recalc_rate,
	.round_rate	= clk_stm32_composite_round_rate,
	.get_parent	= clk_stm32_composite_get_parent,
	.set_parent	= clk_stm32_composite_set_parent,
	.enable		= clk_stm32_composite_gate_enable,
	.disable	= clk_stm32_composite_gate_disable,
	.is_enabled	= clk_stm32_composite_is_enabled,
	.disable_unused	= clk_stm32_composite_disable_unused,
};

u8 clk_stm32_mux_get_parent(struct clk_hw *hw)
{
	struct clk_stm32_mux *mux = to_clk_stm32_mux(hw);

	return clk_stm32_get_parent_mux(mux->base, mux->clock_data,
					mux->mux_id);
}

int clk_stm32_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_stm32_mux *mux = to_clk_stm32_mux(hw);
	unsigned long flags = 0;

	spin_lock_irqsave(mux->lock, flags);

	clk_stm32_set_parent_mux(mux->base, mux->clock_data,
				 mux->mux_id, index);

	spin_unlock_irqrestore(mux->lock, flags);

	return 0;
}

const struct clk_ops clk_stm32_mux_ops = {
	.get_parent	= clk_stm32_mux_get_parent,
	.set_parent	= clk_stm32_mux_set_parent,
};

static int clk_stm32_divider_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct clk_stm32_div *div = to_clk_stm32_divider(hw);
	unsigned long flags = 0;
	int ret;

	if (div->div_id == NO_STM32_DIV)
		return rate;

	spin_lock_irqsave(div->lock, flags);

	ret = clk_stm32_set_rate_divider(div->base, div->clock_data, div->div_id,
					 rate, parent_rate);

	spin_unlock_irqrestore(div->lock, flags);

	return ret;

}

static long clk_stm32_divider_round_rate(struct clk_hw *hw, unsigned long rate,
					 unsigned long *prate)
{
	struct clk_stm32_div *div = to_clk_stm32_divider(hw);
	const struct stm32_div_cfg *divider;

	if (div->div_id == NO_STM32_DIV)
		return rate;

	divider = &div->clock_data->dividers[div->div_id];

	/* if read only, just return current value */
	if (divider->flags & CLK_DIVIDER_READ_ONLY) {
		u32 val;

		val =  readl(div->base + divider->offset) >> divider->shift;
		val &= clk_div_mask(divider->width);

		return divider_ro_round_rate(hw, rate, prate, divider->table,
				divider->width, divider->flags,
				val);
	}

	return divider_round_rate_parent(hw, clk_hw_get_parent(hw),
					 rate, prate, divider->table,
					 divider->width, divider->flags);
}

static unsigned long clk_stm32_divider_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct clk_stm32_div *div = to_clk_stm32_divider(hw);

	if (div->div_id == NO_STM32_DIV)
		return parent_rate;

	return clk_stm32_get_rate_divider(div->base, div->clock_data,
					  div->div_id, parent_rate);
}

const struct clk_ops clk_stm32_divider_ops = {
	.recalc_rate	= clk_stm32_divider_recalc_rate,
	.round_rate	= clk_stm32_divider_round_rate,
	.set_rate	= clk_stm32_divider_set_rate,
};

/* Clock register function */
struct clk_hw *clk_stm32_gate_register(struct device *dev,
				       const struct stm32_rcc_match_data *data,
				       void __iomem *base,
				       spinlock_t *lock,
				       const struct clock_config *cfg)
{
	struct clk_stm32_gate *gate = cfg->clock_cfg;
	struct clk_hw *hw = &gate->hw;
	const struct clk_init_data *init_data = hw->init;
	int err;

	gate->base = base;
	gate->lock = lock;
	gate->clock_data = data->clock_data;

	err = clk_hw_register(dev, hw);
	if (err) {
		dev_err(dev, "Can't register clk %s: %ld\n", init_data->name,  PTR_ERR(hw));
		return ERR_PTR(err);
	}

	return hw;
}

struct clk_hw *clk_stm32_div_register(struct device *dev,
				      const struct stm32_rcc_match_data *data,
				      void __iomem *base,
				      spinlock_t *lock,
				      const struct clock_config *cfg)
{
	struct clk_stm32_div *div = cfg->clock_cfg;
	struct clk_hw *hw = &div->hw;
	const struct clk_init_data *init_data = hw->init;
	int err;

	div->base = base;
	div->lock = lock;
	div->clock_data = data->clock_data;

	err = clk_hw_register(dev, hw);
	if (err) {
		dev_err(dev, "Can't register clk %s: %ld\n", init_data->name,  PTR_ERR(hw));
		return ERR_PTR(err);
	}

	return hw;
}

struct clk_hw *clk_stm32_mux_register(struct device *dev,
				      const struct stm32_rcc_match_data *data,
				      void __iomem *base,
				      spinlock_t *lock,
				      const struct clock_config *cfg)
{
	struct clk_stm32_mux *mux = cfg->clock_cfg;
	struct clk_hw *hw = &mux->hw;
	const struct clk_init_data *init_data = hw->init;
	int err;

	mux->base = base;
	mux->lock = lock;
	mux->clock_data = data->clock_data;

	err = clk_hw_register(dev, hw);
	if (err) {
		dev_err(dev, "Can't register clk %s: %ld\n", init_data->name,  PTR_ERR(hw));
		return ERR_PTR(err);
	}

	return hw;
}

struct clk_hw *clk_stm32_composite_register(struct device *dev,
					    const struct stm32_rcc_match_data *data,
					    void __iomem *base,
					    spinlock_t *lock,
					    const struct clock_config *cfg)
{
	struct clk_stm32_composite *composite = cfg->clock_cfg;
	struct clk_hw *hw = &composite->hw;
	const struct clk_init_data *init_data = hw->init;
	int err;

	composite->base = base;
	composite->lock = lock;
	composite->clock_data = data->clock_data;

	err = clk_hw_register(dev, hw);
	if (err) {
		dev_err(dev, "Can't register clk %s: %ld\n", init_data->name,  PTR_ERR(hw));
		return ERR_PTR(err);
	}

	return hw;
}