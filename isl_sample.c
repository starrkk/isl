#include "isl_sample.h"
#include "isl_sample_piplib.h"
#include "isl_vec.h"
#include "isl_mat.h"
#include "isl_seq.h"
#include "isl_map_private.h"
#include "isl_equalities.h"
#include "isl_tab.h"
#include "isl_basis_reduction.h"

static struct isl_vec *empty_sample(struct isl_basic_set *bset)
{
	struct isl_vec *vec;

	vec = isl_vec_alloc(bset->ctx, 0);
	isl_basic_set_free(bset);
	return vec;
}

/* Construct a zero sample of the same dimension as bset.
 * As a special case, if bset is zero-dimensional, this
 * function creates a zero-dimensional sample point.
 */
static struct isl_vec *zero_sample(struct isl_basic_set *bset)
{
	unsigned dim;
	struct isl_vec *sample;

	dim = isl_basic_set_total_dim(bset);
	sample = isl_vec_alloc(bset->ctx, 1 + dim);
	if (sample) {
		isl_int_set_si(sample->el[0], 1);
		isl_seq_clr(sample->el + 1, dim);
	}
	isl_basic_set_free(bset);
	return sample;
}

static struct isl_vec *interval_sample(struct isl_basic_set *bset)
{
	int i;
	isl_int t;
	struct isl_vec *sample;

	bset = isl_basic_set_simplify(bset);
	if (!bset)
		return NULL;
	if (isl_basic_set_fast_is_empty(bset))
		return empty_sample(bset);
	if (bset->n_eq == 0 && bset->n_ineq == 0)
		return zero_sample(bset);

	sample = isl_vec_alloc(bset->ctx, 2);
	isl_int_set_si(sample->block.data[0], 1);

	if (bset->n_eq > 0) {
		isl_assert(bset->ctx, bset->n_eq == 1, goto error);
		isl_assert(bset->ctx, bset->n_ineq == 0, goto error);
		if (isl_int_is_one(bset->eq[0][1]))
			isl_int_neg(sample->el[1], bset->eq[0][0]);
		else {
			isl_assert(bset->ctx, isl_int_is_negone(bset->eq[0][1]),
				   goto error);
			isl_int_set(sample->el[1], bset->eq[0][0]);
		}
		isl_basic_set_free(bset);
		return sample;
	}

	isl_int_init(t);
	if (isl_int_is_one(bset->ineq[0][1]))
		isl_int_neg(sample->block.data[1], bset->ineq[0][0]);
	else
		isl_int_set(sample->block.data[1], bset->ineq[0][0]);
	for (i = 1; i < bset->n_ineq; ++i) {
		isl_seq_inner_product(sample->block.data,
					bset->ineq[i], 2, &t);
		if (isl_int_is_neg(t))
			break;
	}
	isl_int_clear(t);
	if (i < bset->n_ineq) {
		isl_vec_free(sample);
		return empty_sample(bset);
	}

	isl_basic_set_free(bset);
	return sample;
error:
	isl_basic_set_free(bset);
	isl_vec_free(sample);
	return NULL;
}

static struct isl_mat *independent_bounds(struct isl_basic_set *bset)
{
	int i, j, n;
	struct isl_mat *dirs = NULL;
	struct isl_mat *bounds = NULL;
	unsigned dim;

	if (!bset)
		return NULL;

	dim = isl_basic_set_n_dim(bset);
	bounds = isl_mat_alloc(bset->ctx, 1+dim, 1+dim);
	if (!bounds)
		return NULL;

	isl_int_set_si(bounds->row[0][0], 1);
	isl_seq_clr(bounds->row[0]+1, dim);
	bounds->n_row = 1;

	if (bset->n_ineq == 0)
		return bounds;

	dirs = isl_mat_alloc(bset->ctx, dim, dim);
	if (!dirs) {
		isl_mat_free(bounds);
		return NULL;
	}
	isl_seq_cpy(dirs->row[0], bset->ineq[0]+1, dirs->n_col);
	isl_seq_cpy(bounds->row[1], bset->ineq[0], bounds->n_col);
	for (j = 1, n = 1; n < dim && j < bset->n_ineq; ++j) {
		int pos;

		isl_seq_cpy(dirs->row[n], bset->ineq[j]+1, dirs->n_col);

		pos = isl_seq_first_non_zero(dirs->row[n], dirs->n_col);
		if (pos < 0)
			continue;
		for (i = 0; i < n; ++i) {
			int pos_i;
			pos_i = isl_seq_first_non_zero(dirs->row[i], dirs->n_col);
			if (pos_i < pos)
				continue;
			if (pos_i > pos)
				break;
			isl_seq_elim(dirs->row[n], dirs->row[i], pos,
					dirs->n_col, NULL);
			pos = isl_seq_first_non_zero(dirs->row[n], dirs->n_col);
			if (pos < 0)
				break;
		}
		if (pos < 0)
			continue;
		if (i < n) {
			int k;
			isl_int *t = dirs->row[n];
			for (k = n; k > i; --k)
				dirs->row[k] = dirs->row[k-1];
			dirs->row[i] = t;
		}
		++n;
		isl_seq_cpy(bounds->row[n], bset->ineq[j], bounds->n_col);
	}
	isl_mat_free(dirs);
	bounds->n_row = 1+n;
	return bounds;
}

static void swap_inequality(struct isl_basic_set *bset, int a, int b)
{
	isl_int *t = bset->ineq[a];
	bset->ineq[a] = bset->ineq[b];
	bset->ineq[b] = t;
}

/* Skew into positive orthant and project out lineality space.
 *
 * We perform a unimodular transformation that turns a selected
 * maximal set of linearly independent bounds into constraints
 * on the first dimensions that impose that these first dimensions
 * are non-negative.  In particular, the constraint matrix is lower
 * triangular with positive entries on the diagonal and negative
 * entries below.
 * If "bset" has a lineality space then these constraints (and therefore
 * all constraints in bset) only involve the first dimensions.
 * The remaining dimensions then do not appear in any constraints and
 * we can select any value for them, say zero.  We therefore project
 * out this final dimensions and plug in the value zero later.  This
 * is accomplished by simply dropping the final columns of
 * the unimodular transformation.
 */
static struct isl_basic_set *isl_basic_set_skew_to_positive_orthant(
	struct isl_basic_set *bset, struct isl_mat **T)
{
	struct isl_mat *U = NULL;
	struct isl_mat *bounds = NULL;
	int i, j;
	unsigned old_dim, new_dim;

	*T = NULL;
	if (!bset)
		return NULL;

	isl_assert(bset->ctx, isl_basic_set_n_param(bset) == 0, goto error);
	isl_assert(bset->ctx, bset->n_div == 0, goto error);
	isl_assert(bset->ctx, bset->n_eq == 0, goto error);
	
	old_dim = isl_basic_set_n_dim(bset);
	/* Try to move (multiples of) unit rows up. */
	for (i = 0, j = 0; i < bset->n_ineq; ++i) {
		int pos = isl_seq_first_non_zero(bset->ineq[i]+1, old_dim);
		if (pos < 0)
			continue;
		if (isl_seq_first_non_zero(bset->ineq[i]+1+pos+1,
						old_dim-pos-1) >= 0)
			continue;
		if (i != j)
			swap_inequality(bset, i, j);
		++j;
	}
	bounds = independent_bounds(bset);
	if (!bounds)
		goto error;
	new_dim = bounds->n_row - 1;
	bounds = isl_mat_left_hermite(bounds, 1, &U, NULL);
	if (!bounds)
		goto error;
	U = isl_mat_drop_cols(U, 1 + new_dim, old_dim - new_dim);
	bset = isl_basic_set_preimage(bset, isl_mat_copy(U));
	if (!bset)
		goto error;
	*T = U;
	isl_mat_free(bounds);
	return bset;
error:
	isl_mat_free(bounds);
	isl_mat_free(U);
	isl_basic_set_free(bset);
	return NULL;
}

/* Find a sample integer point, if any, in bset, which is known
 * to have equalities.  If bset contains no integer points, then
 * return a zero-length vector.
 * We simply remove the known equalities, compute a sample
 * in the resulting bset, using the specified recurse function,
 * and then transform the sample back to the original space.
 */
static struct isl_vec *sample_eq(struct isl_basic_set *bset,
	struct isl_vec *(*recurse)(struct isl_basic_set *))
{
	struct isl_mat *T;
	struct isl_vec *sample;

	if (!bset)
		return NULL;

	bset = isl_basic_set_remove_equalities(bset, &T, NULL);
	sample = recurse(bset);
	if (!sample || sample->size == 0)
		isl_mat_free(T);
	else
		sample = isl_mat_vec_product(T, sample);
	return sample;
}

/* Return a matrix containing the equalities of the tableau
 * in constraint form.  The tableau is assumed to have
 * an associated bset that has been kept up-to-date.
 */
static struct isl_mat *tab_equalities(struct isl_tab *tab)
{
	int i, j;
	int n_eq;
	struct isl_mat *eq;
	struct isl_basic_set *bset;

	if (!tab)
		return NULL;

	bset = isl_tab_peek_bset(tab);
	isl_assert(tab->mat->ctx, bset, return NULL);

	n_eq = tab->n_var - tab->n_col + tab->n_dead;
	if (tab->empty || n_eq == 0)
		return isl_mat_alloc(tab->mat->ctx, 0, tab->n_var);
	if (n_eq == tab->n_var)
		return isl_mat_identity(tab->mat->ctx, tab->n_var);

	eq = isl_mat_alloc(tab->mat->ctx, n_eq, tab->n_var);
	if (!eq)
		return NULL;
	for (i = 0, j = 0; i < tab->n_con; ++i) {
		if (tab->con[i].is_row)
			continue;
		if (tab->con[i].index >= 0 && tab->con[i].index >= tab->n_dead)
			continue;
		if (i < bset->n_eq)
			isl_seq_cpy(eq->row[j], bset->eq[i] + 1, tab->n_var);
		else
			isl_seq_cpy(eq->row[j],
				    bset->ineq[i - bset->n_eq] + 1, tab->n_var);
		++j;
	}
	isl_assert(bset->ctx, j == n_eq, goto error);
	return eq;
error:
	isl_mat_free(eq);
	return NULL;
}

/* Compute and return an initial basis for the bounded tableau "tab".
 *
 * If the tableau is either full-dimensional or zero-dimensional,
 * the we simply return an identity matrix.
 * Otherwise, we construct a basis whose first directions correspond
 * to equalities.
 */
static struct isl_mat *initial_basis(struct isl_tab *tab)
{
	int n_eq;
	struct isl_mat *eq;
	struct isl_mat *Q;

	n_eq = tab->n_var - tab->n_col + tab->n_dead;
	if (tab->empty || n_eq == 0 || n_eq == tab->n_var)
		return isl_mat_identity(tab->mat->ctx, 1 + tab->n_var);

	eq = tab_equalities(tab);
	eq = isl_mat_left_hermite(eq, 0, NULL, &Q);
	if (!eq)
		return NULL;
	isl_mat_free(eq);

	Q = isl_mat_lin_to_aff(Q);
	return Q;
}

/* Given a tableau representing a set, find and return
 * an integer point in the set, if there is any.
 *
 * We perform a depth first search
 * for an integer point, by scanning all possible values in the range
 * attained by a basis vector, where an initial basis may have been set
 * by the calling function.  Otherwise an initial basis that exploits
 * the equalities in the tableau is created.
 * tab->n_zero is currently ignored and is clobbered by this function.
 *
 * The tableau is allowed to have unbounded direction, but then
 * the calling function needs to set an initial basis, with the
 * unbounded directions last and with tab->n_unbounded set
 * to the number of unbounded directions.
 * Furthermore, the calling functions needs to add shifted copies
 * of all constraints involving unbounded directions to ensure
 * that any feasible rational value in these directions can be rounded
 * up to yield a feasible integer value.
 * In particular, let B define the given basis x' = B x
 * and let T be the inverse of B, i.e., X = T x'.
 * Let a x + c >= 0 be a constraint of the set represented by the tableau,
 * or a T x' + c >= 0 in terms of the given basis.  Assume that
 * the bounded directions have an integer value, then we can safely
 * round up the values for the unbounded directions if we make sure
 * that x' not only satisfies the original constraint, but also
 * the constraint "a T x' + c + s >= 0" with s the sum of all
 * negative values in the last n_unbounded entries of "a T".
 * The calling function therefore needs to add the constraint
 * a x + c + s >= 0.  The current function then scans the first
 * directions for an integer value and once those have been found,
 * it can compute "T ceil(B x)" to yield an integer point in the set.
 * Note that during the search, the first rows of B may be changed
 * by a basis reduction, but the last n_unbounded rows of B remain
 * unaltered and are also not mixed into the first rows.
 *
 * The search is implemented iteratively.  "level" identifies the current
 * basis vector.  "init" is true if we want the first value at the current
 * level and false if we want the next value.
 *
 * The initial basis is the identity matrix.  If the range in some direction
 * contains more than one integer value, we perform basis reduction based
 * on the value of ctx->opt->gbr
 *	- ISL_GBR_NEVER:	never perform basis reduction
 *	- ISL_GBR_ONCE:		only perform basis reduction the first
 *				time such a range is encountered
 *	- ISL_GBR_ALWAYS:	always perform basis reduction when
 *				such a range is encountered
 *
 * When ctx->opt->gbr is set to ISL_GBR_ALWAYS, then we allow the basis
 * reduction computation to return early.  That is, as soon as it
 * finds a reasonable first direction.
 */ 
struct isl_vec *isl_tab_sample(struct isl_tab *tab)
{
	unsigned dim;
	unsigned gbr;
	struct isl_ctx *ctx;
	struct isl_vec *sample;
	struct isl_vec *min;
	struct isl_vec *max;
	enum isl_lp_result res;
	int level;
	int init;
	int reduced;
	struct isl_tab_undo **snap;

	if (!tab)
		return NULL;
	if (tab->empty)
		return isl_vec_alloc(tab->mat->ctx, 0);

	if (!tab->basis)
		tab->basis = initial_basis(tab);
	if (!tab->basis)
		return NULL;
	isl_assert(tab->mat->ctx, tab->basis->n_row == tab->n_var + 1,
		    return NULL);
	isl_assert(tab->mat->ctx, tab->basis->n_col == tab->n_var + 1,
		    return NULL);

	ctx = tab->mat->ctx;
	dim = tab->n_var;
	gbr = ctx->opt->gbr;

	if (tab->n_unbounded == tab->n_var) {
		sample = isl_tab_get_sample_value(tab);
		sample = isl_mat_vec_product(isl_mat_copy(tab->basis), sample);
		sample = isl_vec_ceil(sample);
		sample = isl_mat_vec_inverse_product(isl_mat_copy(tab->basis),
							sample);
		return sample;
	}

	if (isl_tab_extend_cons(tab, dim + 1) < 0)
		return NULL;

	min = isl_vec_alloc(ctx, dim);
	max = isl_vec_alloc(ctx, dim);
	snap = isl_alloc_array(ctx, struct isl_tab_undo *, dim);

	if (!min || !max || !snap)
		goto error;

	level = 0;
	init = 1;
	reduced = 0;

	while (level >= 0) {
		int empty = 0;
		if (init) {
			res = isl_tab_min(tab, tab->basis->row[1 + level],
				    ctx->one, &min->el[level], NULL, 0);
			if (res == isl_lp_empty)
				empty = 1;
			isl_assert(ctx, res != isl_lp_unbounded, goto error);
			if (res == isl_lp_error)
				goto error;
			if (!empty && isl_tab_sample_is_integer(tab))
				break;
			isl_seq_neg(tab->basis->row[1 + level] + 1,
				    tab->basis->row[1 + level] + 1, dim);
			res = isl_tab_min(tab, tab->basis->row[1 + level],
				    ctx->one, &max->el[level], NULL, 0);
			isl_seq_neg(tab->basis->row[1 + level] + 1,
				    tab->basis->row[1 + level] + 1, dim);
			isl_int_neg(max->el[level], max->el[level]);
			if (res == isl_lp_empty)
				empty = 1;
			isl_assert(ctx, res != isl_lp_unbounded, goto error);
			if (res == isl_lp_error)
				goto error;
			if (!empty && isl_tab_sample_is_integer(tab))
				break;
			if (!empty && !reduced &&
			    ctx->opt->gbr != ISL_GBR_NEVER &&
			    isl_int_lt(min->el[level], max->el[level])) {
				unsigned gbr_only_first;
				if (ctx->opt->gbr == ISL_GBR_ONCE)
					ctx->opt->gbr = ISL_GBR_NEVER;
				tab->n_zero = level;
				gbr_only_first = ctx->opt->gbr_only_first;
				ctx->opt->gbr_only_first =
					ctx->opt->gbr == ISL_GBR_ALWAYS;
				tab = isl_tab_compute_reduced_basis(tab);
				ctx->opt->gbr_only_first = gbr_only_first;
				if (!tab || !tab->basis)
					goto error;
				reduced = 1;
				continue;
			}
			reduced = 0;
			snap[level] = isl_tab_snap(tab);
		} else
			isl_int_add_ui(min->el[level], min->el[level], 1);

		if (empty || isl_int_gt(min->el[level], max->el[level])) {
			level--;
			init = 0;
			if (level >= 0)
				if (isl_tab_rollback(tab, snap[level]) < 0)
					goto error;
			continue;
		}
		isl_int_neg(tab->basis->row[1 + level][0], min->el[level]);
		tab = isl_tab_add_valid_eq(tab, tab->basis->row[1 + level]);
		isl_int_set_si(tab->basis->row[1 + level][0], 0);
		if (level + tab->n_unbounded < dim - 1) {
			++level;
			init = 1;
			continue;
		}
		break;
	}

	if (level >= 0) {
		sample = isl_tab_get_sample_value(tab);
		if (!sample)
			goto error;
		if (tab->n_unbounded && !isl_int_is_one(sample->el[0])) {
			sample = isl_mat_vec_product(isl_mat_copy(tab->basis),
						     sample);
			sample = isl_vec_ceil(sample);
			sample = isl_mat_vec_inverse_product(
					isl_mat_copy(tab->basis), sample);
		}
	} else
		sample = isl_vec_alloc(ctx, 0);

	ctx->opt->gbr = gbr;
	isl_vec_free(min);
	isl_vec_free(max);
	free(snap);
	return sample;
error:
	ctx->opt->gbr = gbr;
	isl_vec_free(min);
	isl_vec_free(max);
	free(snap);
	return NULL;
}

/* Given a basic set that is known to be bounded, find and return
 * an integer point in the basic set, if there is any.
 *
 * After handling some trivial cases, we construct a tableau
 * and then use isl_tab_sample to find a sample, passing it
 * the identity matrix as initial basis.
 */ 
static struct isl_vec *sample_bounded(struct isl_basic_set *bset)
{
	unsigned dim;
	struct isl_ctx *ctx;
	struct isl_vec *sample;
	struct isl_tab *tab = NULL;

	if (!bset)
		return NULL;

	if (isl_basic_set_fast_is_empty(bset))
		return empty_sample(bset);

	dim = isl_basic_set_total_dim(bset);
	if (dim == 0)
		return zero_sample(bset);
	if (dim == 1)
		return interval_sample(bset);
	if (bset->n_eq > 0)
		return sample_eq(bset, sample_bounded);

	ctx = bset->ctx;

	tab = isl_tab_from_basic_set(bset);
	if (tab && tab->empty) {
		isl_tab_free(tab);
		ISL_F_SET(bset, ISL_BASIC_SET_EMPTY);
		sample = isl_vec_alloc(bset->ctx, 0);
		isl_basic_set_free(bset);
		return sample;
	}

	if (isl_tab_track_bset(tab, isl_basic_set_copy(bset)) < 0)
		goto error;
	if (!ISL_F_ISSET(bset, ISL_BASIC_SET_NO_IMPLICIT))
		tab = isl_tab_detect_implicit_equalities(tab);
	if (!tab)
		goto error;

	sample = isl_tab_sample(tab);
	if (!sample)
		goto error;

	if (sample->size > 0) {
		isl_vec_free(bset->sample);
		bset->sample = isl_vec_copy(sample);
	}

	isl_basic_set_free(bset);
	isl_tab_free(tab);
	return sample;
error:
	isl_basic_set_free(bset);
	isl_tab_free(tab);
	return NULL;
}

/* Given a basic set "bset" and a value "sample" for the first coordinates
 * of bset, plug in these values and drop the corresponding coordinates.
 *
 * We do this by computing the preimage of the transformation
 *
 *	     [ 1 0 ]
 *	x =  [ s 0 ] x'
 *	     [ 0 I ]
 *
 * where [1 s] is the sample value and I is the identity matrix of the
 * appropriate dimension.
 */
static struct isl_basic_set *plug_in(struct isl_basic_set *bset,
	struct isl_vec *sample)
{
	int i;
	unsigned total;
	struct isl_mat *T;

	if (!bset || !sample)
		goto error;

	total = isl_basic_set_total_dim(bset);
	T = isl_mat_alloc(bset->ctx, 1 + total, 1 + total - (sample->size - 1));
	if (!T)
		goto error;

	for (i = 0; i < sample->size; ++i) {
		isl_int_set(T->row[i][0], sample->el[i]);
		isl_seq_clr(T->row[i] + 1, T->n_col - 1);
	}
	for (i = 0; i < T->n_col - 1; ++i) {
		isl_seq_clr(T->row[sample->size + i], T->n_col);
		isl_int_set_si(T->row[sample->size + i][1 + i], 1);
	}
	isl_vec_free(sample);

	bset = isl_basic_set_preimage(bset, T);
	return bset;
error:
	isl_basic_set_free(bset);
	isl_vec_free(sample);
	return NULL;
}

/* Given a basic set "bset", return any (possibly non-integer) point
 * in the basic set.
 */
static struct isl_vec *rational_sample(struct isl_basic_set *bset)
{
	struct isl_tab *tab;
	struct isl_vec *sample;

	if (!bset)
		return NULL;

	tab = isl_tab_from_basic_set(bset);
	sample = isl_tab_get_sample_value(tab);
	isl_tab_free(tab);

	isl_basic_set_free(bset);

	return sample;
}

/* Given a linear cone "cone" and a rational point "vec",
 * construct a polyhedron with shifted copies of the constraints in "cone",
 * i.e., a polyhedron with "cone" as its recession cone, such that each
 * point x in this polyhedron is such that the unit box positioned at x
 * lies entirely inside the affine cone 'vec + cone'.
 * Any rational point in this polyhedron may therefore be rounded up
 * to yield an integer point that lies inside said affine cone.
 *
 * Denote the constraints of cone by "<a_i, x> >= 0" and the rational
 * point "vec" by v/d.
 * Let b_i = <a_i, v>.  Then the affine cone 'vec + cone' is given
 * by <a_i, x> - b/d >= 0.
 * The polyhedron <a_i, x> - ceil{b/d} >= 0 is a subset of this affine cone.
 * We prefer this polyhedron over the actual affine cone because it doesn't
 * require a scaling of the constraints.
 * If each of the vertices of the unit cube positioned at x lies inside
 * this polyhedron, then the whole unit cube at x lies inside the affine cone.
 * We therefore impose that x' = x + \sum e_i, for any selection of unit
 * vectors lies inside the polyhedron, i.e.,
 *
 *	<a_i, x'> - ceil{b/d} = <a_i, x> + sum a_i - ceil{b/d} >= 0
 *
 * The most stringent of these constraints is the one that selects
 * all negative a_i, so the polyhedron we are looking for has constraints
 *
 *	<a_i, x> + sum_{a_i < 0} a_i - ceil{b/d} >= 0
 *
 * Note that if cone were known to have only non-negative rays
 * (which can be accomplished by a unimodular transformation),
 * then we would only have to check the points x' = x + e_i
 * and we only have to add the smallest negative a_i (if any)
 * instead of the sum of all negative a_i.
 */
static struct isl_basic_set *shift_cone(struct isl_basic_set *cone,
	struct isl_vec *vec)
{
	int i, j, k;
	unsigned total;

	struct isl_basic_set *shift = NULL;

	if (!cone || !vec)
		goto error;

	isl_assert(cone->ctx, cone->n_eq == 0, goto error);

	total = isl_basic_set_total_dim(cone);

	shift = isl_basic_set_alloc_dim(isl_basic_set_get_dim(cone),
					0, 0, cone->n_ineq);

	for (i = 0; i < cone->n_ineq; ++i) {
		k = isl_basic_set_alloc_inequality(shift);
		if (k < 0)
			goto error;
		isl_seq_cpy(shift->ineq[k] + 1, cone->ineq[i] + 1, total);
		isl_seq_inner_product(shift->ineq[k] + 1, vec->el + 1, total,
				      &shift->ineq[k][0]);
		isl_int_cdiv_q(shift->ineq[k][0],
			       shift->ineq[k][0], vec->el[0]);
		isl_int_neg(shift->ineq[k][0], shift->ineq[k][0]);
		for (j = 0; j < total; ++j) {
			if (isl_int_is_nonneg(shift->ineq[k][1 + j]))
				continue;
			isl_int_add(shift->ineq[k][0],
				    shift->ineq[k][0], shift->ineq[k][1 + j]);
		}
	}

	isl_basic_set_free(cone);
	isl_vec_free(vec);

	return isl_basic_set_finalize(shift);
error:
	isl_basic_set_free(shift);
	isl_basic_set_free(cone);
	isl_vec_free(vec);
	return NULL;
}

/* Given a rational point vec in a (transformed) basic set,
 * such that cone is the recession cone of the original basic set,
 * "round up" the rational point to an integer point.
 *
 * We first check if the rational point just happens to be integer.
 * If not, we transform the cone in the same way as the basic set,
 * pick a point x in this cone shifted to the rational point such that
 * the whole unit cube at x is also inside this affine cone.
 * Then we simply round up the coordinates of x and return the
 * resulting integer point.
 */
static struct isl_vec *round_up_in_cone(struct isl_vec *vec,
	struct isl_basic_set *cone, struct isl_mat *U)
{
	unsigned total;

	if (!vec || !cone || !U)
		goto error;

	isl_assert(vec->ctx, vec->size != 0, goto error);
	if (isl_int_is_one(vec->el[0])) {
		isl_mat_free(U);
		isl_basic_set_free(cone);
		return vec;
	}

	total = isl_basic_set_total_dim(cone);
	cone = isl_basic_set_preimage(cone, U);
	cone = isl_basic_set_remove_dims(cone, 0, total - (vec->size - 1));

	cone = shift_cone(cone, vec);

	vec = rational_sample(cone);
	vec = isl_vec_ceil(vec);
	return vec;
error:
	isl_mat_free(U);
	isl_vec_free(vec);
	isl_basic_set_free(cone);
	return NULL;
}

/* Concatenate two integer vectors, i.e., two vectors with denominator
 * (stored in element 0) equal to 1.
 */
static struct isl_vec *vec_concat(struct isl_vec *vec1, struct isl_vec *vec2)
{
	struct isl_vec *vec;

	if (!vec1 || !vec2)
		goto error;
	isl_assert(vec1->ctx, vec1->size > 0, goto error);
	isl_assert(vec2->ctx, vec2->size > 0, goto error);
	isl_assert(vec1->ctx, isl_int_is_one(vec1->el[0]), goto error);
	isl_assert(vec2->ctx, isl_int_is_one(vec2->el[0]), goto error);

	vec = isl_vec_alloc(vec1->ctx, vec1->size + vec2->size - 1);
	if (!vec)
		goto error;

	isl_seq_cpy(vec->el, vec1->el, vec1->size);
	isl_seq_cpy(vec->el + vec1->size, vec2->el + 1, vec2->size - 1);

	isl_vec_free(vec1);
	isl_vec_free(vec2);

	return vec;
error:
	isl_vec_free(vec1);
	isl_vec_free(vec2);
	return NULL;
}

/* Drop all constraints in bset that involve any of the dimensions
 * first to first+n-1.
 */
static struct isl_basic_set *drop_constraints_involving
	(struct isl_basic_set *bset, unsigned first, unsigned n)
{
	int i;

	if (!bset)
		return NULL;

	bset = isl_basic_set_cow(bset);

	for (i = bset->n_ineq - 1; i >= 0; --i) {
		if (isl_seq_first_non_zero(bset->ineq[i] + 1 + first, n) == -1)
			continue;
		isl_basic_set_drop_inequality(bset, i);
	}

	return bset;
}

/* Give a basic set "bset" with recession cone "cone", compute and
 * return an integer point in bset, if any.
 *
 * If the recession cone is full-dimensional, then we know that
 * bset contains an infinite number of integer points and it is
 * fairly easy to pick one of them.
 * If the recession cone is not full-dimensional, then we first
 * transform bset such that the bounded directions appear as
 * the first dimensions of the transformed basic set.
 * We do this by using a unimodular transformation that transforms
 * the equalities in the recession cone to equalities on the first
 * dimensions.
 *
 * The transformed set is then projected onto its bounded dimensions.
 * Note that to compute this projection, we can simply drop all constraints
 * involving any of the unbounded dimensions since these constraints
 * cannot be combined to produce a constraint on the bounded dimensions.
 * To see this, assume that there is such a combination of constraints
 * that produces a constraint on the bounded dimensions.  This means
 * that some combination of the unbounded dimensions has both an upper
 * bound and a lower bound in terms of the bounded dimensions, but then
 * this combination would be a bounded direction too and would have been
 * transformed into a bounded dimensions.
 *
 * We then compute a sample value in the bounded dimensions.
 * If no such value can be found, then the original set did not contain
 * any integer points and we are done.
 * Otherwise, we plug in the value we found in the bounded dimensions,
 * project out these bounded dimensions and end up with a set with
 * a full-dimensional recession cone.
 * A sample point in this set is computed by "rounding up" any
 * rational point in the set.
 *
 * The sample points in the bounded and unbounded dimensions are
 * then combined into a single sample point and transformed back
 * to the original space.
 */
__isl_give isl_vec *isl_basic_set_sample_with_cone(
	__isl_take isl_basic_set *bset, __isl_take isl_basic_set *cone)
{
	unsigned total;
	unsigned cone_dim;
	struct isl_mat *M, *U;
	struct isl_vec *sample;
	struct isl_vec *cone_sample;
	struct isl_ctx *ctx;
	struct isl_basic_set *bounded;

	if (!bset || !cone)
		goto error;

	ctx = bset->ctx;
	total = isl_basic_set_total_dim(cone);
	cone_dim = total - cone->n_eq;

	M = isl_mat_sub_alloc(bset->ctx, cone->eq, 0, cone->n_eq, 1, total);
	M = isl_mat_left_hermite(M, 0, &U, NULL);
	if (!M)
		goto error;
	isl_mat_free(M);

	U = isl_mat_lin_to_aff(U);
	bset = isl_basic_set_preimage(bset, isl_mat_copy(U));

	bounded = isl_basic_set_copy(bset);
	bounded = drop_constraints_involving(bounded, total - cone_dim, cone_dim);
	bounded = isl_basic_set_drop_dims(bounded, total - cone_dim, cone_dim);
	sample = sample_bounded(bounded);
	if (!sample || sample->size == 0) {
		isl_basic_set_free(bset);
		isl_basic_set_free(cone);
		isl_mat_free(U);
		return sample;
	}
	bset = plug_in(bset, isl_vec_copy(sample));
	cone_sample = rational_sample(bset);
	cone_sample = round_up_in_cone(cone_sample, cone, isl_mat_copy(U));
	sample = vec_concat(sample, cone_sample);
	sample = isl_mat_vec_product(U, sample);
	return sample;
error:
	isl_basic_set_free(cone);
	isl_basic_set_free(bset);
	return NULL;
}

static void vec_sum_of_neg(struct isl_vec *v, isl_int *s)
{
	int i;

	isl_int_set_si(*s, 0);

	for (i = 0; i < v->size; ++i)
		if (isl_int_is_neg(v->el[i]))
			isl_int_add(*s, *s, v->el[i]);
}

/* Given a tableau "tab", a tableau "tab_cone" that corresponds
 * to the recession cone and the inverse of a new basis U = inv(B),
 * with the unbounded directions in B last,
 * add constraints to "tab" that ensure any rational value
 * in the unbounded directions can be rounded up to an integer value.
 *
 * The new basis is given by x' = B x, i.e., x = U x'.
 * For any rational value of the last tab->n_unbounded coordinates
 * in the update tableau, the value that is obtained by rounding
 * up this value should be contained in the original tableau.
 * For any constraint "a x + c >= 0", we therefore need to add
 * a constraint "a x + c + s >= 0", with s the sum of all negative
 * entries in the last elements of "a U".
 *
 * Since we are not interested in the first entries of any of the "a U",
 * we first drop the columns of U that correpond to bounded directions.
 */
static int tab_shift_cone(struct isl_tab *tab,
	struct isl_tab *tab_cone, struct isl_mat *U)
{
	int i;
	isl_int v;
	struct isl_basic_set *bset = NULL;

	if (tab && tab->n_unbounded == 0) {
		isl_mat_free(U);
		return 0;
	}
	isl_int_init(v);
	if (!tab || !tab_cone || !U)
		goto error;
	bset = isl_tab_peek_bset(tab_cone);
	U = isl_mat_drop_cols(U, 0, tab->n_var - tab->n_unbounded);
	for (i = 0; i < bset->n_ineq; ++i) {
		int ok;
		struct isl_vec *row = NULL;
		if (isl_tab_is_equality(tab_cone, tab_cone->n_eq + i))
			continue;
		row = isl_vec_alloc(bset->ctx, tab_cone->n_var);
		if (!row)
			goto error;
		isl_seq_cpy(row->el, bset->ineq[i] + 1, tab_cone->n_var);
		row = isl_vec_mat_product(row, isl_mat_copy(U));
		if (!row)
			goto error;
		vec_sum_of_neg(row, &v);
		isl_vec_free(row);
		if (isl_int_is_zero(v))
			continue;
		tab = isl_tab_extend(tab, 1);
		isl_int_add(bset->ineq[i][0], bset->ineq[i][0], v);
		ok = isl_tab_add_ineq(tab, bset->ineq[i]) >= 0;
		isl_int_sub(bset->ineq[i][0], bset->ineq[i][0], v);
		if (!ok)
			goto error;
	}

	isl_mat_free(U);
	isl_int_clear(v);
	return 0;
error:
	isl_mat_free(U);
	isl_int_clear(v);
	return -1;
}

/* Compute and return an initial basis for the possibly
 * unbounded tableau "tab".  "tab_cone" is a tableau
 * for the corresponding recession cone.
 * Additionally, add constraints to "tab" that ensure
 * that any rational value for the unbounded directions
 * can be rounded up to an integer value.
 *
 * If the tableau is bounded, i.e., if the recession cone
 * is zero-dimensional, then we just use inital_basis.
 * Otherwise, we construct a basis whose first directions
 * correspond to equalities, followed by bounded directions,
 * i.e., equalities in the recession cone.
 * The remaining directions are then unbounded.
 */
int isl_tab_set_initial_basis_with_cone(struct isl_tab *tab,
	struct isl_tab *tab_cone)
{
	struct isl_mat *eq;
	struct isl_mat *cone_eq;
	struct isl_mat *U, *Q;

	if (!tab || !tab_cone)
		return -1;

	if (tab_cone->n_col == tab_cone->n_dead) {
		tab->basis = initial_basis(tab);
		return tab->basis ? 0 : -1;
	}

	eq = tab_equalities(tab);
	if (!eq)
		return -1;
	tab->n_zero = eq->n_row;
	cone_eq = tab_equalities(tab_cone);
	eq = isl_mat_concat(eq, cone_eq);
	if (!eq)
		return -1;
	tab->n_unbounded = tab->n_var - (eq->n_row - tab->n_zero);
	eq = isl_mat_left_hermite(eq, 0, &U, &Q);
	if (!eq)
		return -1;
	isl_mat_free(eq);
	tab->basis = isl_mat_lin_to_aff(Q);
	if (tab_shift_cone(tab, tab_cone, U) < 0)
		return -1;
	if (!tab->basis)
		return -1;
	return 0;
}

/* Compute and return a sample point in bset using generalized basis
 * reduction.  We first check if the input set has a non-trivial
 * recession cone.  If so, we perform some extra preprocessing in
 * sample_with_cone.  Otherwise, we directly perform generalized basis
 * reduction.
 */
static struct isl_vec *gbr_sample(struct isl_basic_set *bset)
{
	unsigned dim;
	struct isl_basic_set *cone;

	dim = isl_basic_set_total_dim(bset);

	cone = isl_basic_set_recession_cone(isl_basic_set_copy(bset));

	if (cone->n_eq < dim)
		return isl_basic_set_sample_with_cone(bset, cone);

	isl_basic_set_free(cone);
	return sample_bounded(bset);
}

static struct isl_vec *pip_sample(struct isl_basic_set *bset)
{
	struct isl_mat *T;
	struct isl_ctx *ctx;
	struct isl_vec *sample;

	bset = isl_basic_set_skew_to_positive_orthant(bset, &T);
	if (!bset)
		return NULL;

	ctx = bset->ctx;
	sample = isl_pip_basic_set_sample(bset);

	if (sample && sample->size != 0)
		sample = isl_mat_vec_product(T, sample);
	else
		isl_mat_free(T);

	return sample;
}

static struct isl_vec *basic_set_sample(struct isl_basic_set *bset, int bounded)
{
	struct isl_ctx *ctx;
	unsigned dim;
	if (!bset)
		return NULL;

	ctx = bset->ctx;
	if (isl_basic_set_fast_is_empty(bset))
		return empty_sample(bset);

	dim = isl_basic_set_n_dim(bset);
	isl_assert(ctx, isl_basic_set_n_param(bset) == 0, goto error);
	isl_assert(ctx, bset->n_div == 0, goto error);

	if (bset->sample && bset->sample->size == 1 + dim) {
		int contains = isl_basic_set_contains(bset, bset->sample);
		if (contains < 0)
			goto error;
		if (contains) {
			struct isl_vec *sample = isl_vec_copy(bset->sample);
			isl_basic_set_free(bset);
			return sample;
		}
	}
	isl_vec_free(bset->sample);
	bset->sample = NULL;

	if (bset->n_eq > 0)
		return sample_eq(bset, bounded ? isl_basic_set_sample_bounded
					       : isl_basic_set_sample_vec);
	if (dim == 0)
		return zero_sample(bset);
	if (dim == 1)
		return interval_sample(bset);

	switch (bset->ctx->opt->ilp_solver) {
	case ISL_ILP_PIP:
		return pip_sample(bset);
	case ISL_ILP_GBR:
		return bounded ? sample_bounded(bset) : gbr_sample(bset);
	}
	isl_assert(bset->ctx, 0, );
error:
	isl_basic_set_free(bset);
	return NULL;
}

__isl_give isl_vec *isl_basic_set_sample_vec(__isl_take isl_basic_set *bset)
{
	return basic_set_sample(bset, 0);
}

/* Compute an integer sample in "bset", where the caller guarantees
 * that "bset" is bounded.
 */
struct isl_vec *isl_basic_set_sample_bounded(struct isl_basic_set *bset)
{
	return basic_set_sample(bset, 1);
}

__isl_give isl_basic_set *isl_basic_set_from_vec(__isl_take isl_vec *vec)
{
	int i;
	int k;
	struct isl_basic_set *bset = NULL;
	struct isl_ctx *ctx;
	unsigned dim;

	if (!vec)
		return NULL;
	ctx = vec->ctx;
	isl_assert(ctx, vec->size != 0, goto error);

	bset = isl_basic_set_alloc(ctx, 0, vec->size - 1, 0, vec->size - 1, 0);
	if (!bset)
		goto error;
	dim = isl_basic_set_n_dim(bset);
	for (i = dim - 1; i >= 0; --i) {
		k = isl_basic_set_alloc_equality(bset);
		if (k < 0)
			goto error;
		isl_seq_clr(bset->eq[k], 1 + dim);
		isl_int_neg(bset->eq[k][0], vec->el[1 + i]);
		isl_int_set(bset->eq[k][1 + i], vec->el[0]);
	}
	bset->sample = vec;

	return bset;
error:
	isl_basic_set_free(bset);
	isl_vec_free(vec);
	return NULL;
}

__isl_give isl_basic_map *isl_basic_map_sample(__isl_take isl_basic_map *bmap)
{
	struct isl_basic_set *bset;
	struct isl_vec *sample_vec;

	bset = isl_basic_map_underlying_set(isl_basic_map_copy(bmap));
	sample_vec = isl_basic_set_sample_vec(bset);
	if (!sample_vec)
		goto error;
	if (sample_vec->size == 0) {
		struct isl_basic_map *sample;
		sample = isl_basic_map_empty_like(bmap);
		isl_vec_free(sample_vec);
		isl_basic_map_free(bmap);
		return sample;
	}
	bset = isl_basic_set_from_vec(sample_vec);
	return isl_basic_map_overlying_set(bset, bmap);
error:
	isl_basic_map_free(bmap);
	return NULL;
}

__isl_give isl_basic_map *isl_map_sample(__isl_take isl_map *map)
{
	int i;
	isl_basic_map *sample = NULL;

	if (!map)
		goto error;

	for (i = 0; i < map->n; ++i) {
		sample = isl_basic_map_sample(isl_basic_map_copy(map->p[i]));
		if (!sample)
			goto error;
		if (!ISL_F_ISSET(sample, ISL_BASIC_MAP_EMPTY))
			break;
		isl_basic_map_free(sample);
	}
	if (i == map->n)
		sample = isl_basic_map_empty_like_map(map);
	isl_map_free(map);
	return sample;
error:
	isl_map_free(map);
	return NULL;
}

__isl_give isl_basic_set *isl_set_sample(__isl_take isl_set *set)
{
	return (isl_basic_set *) isl_map_sample((isl_map *)set);
}
