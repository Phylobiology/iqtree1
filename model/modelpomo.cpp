#include "modelpomo.h"
#include "modeldna.h"
#include <stdlib.h>
#include <assert.h>
#include <string.h>

ModelPoMo::ModelPoMo(const char *model_name,
                     string model_params,
                     StateFreqType freq_type,
                     string freq_params,
                     PhyloTree *tree,
                     bool is_reversible)
    // Do not count rates; does not make sense for PoMo.
    : ModelGTR(tree, false) {
	init(model_name, model_params, freq_type, freq_params, is_reversible);
}

void ModelPoMo::init(const char *model_name,
                     string model_params,
                     StateFreqType freq_type,
                     string freq_params,
                     bool is_reversible) {
    // Check num_states (set in Alignment::readCountsFormat()).
	N = phylo_tree->aln->virtual_pop_size;
    nnuc = 4;
    assert(num_states == (nnuc + (nnuc*(nnuc-1)/2 * (N-1))) );

    if (is_reversible != true) throw "Non-reversible PoMo not supported yet.";

    // Get DNA model info from model_name.  Use ModelDNA for this
    // purpose.  It acts as the basis of the `ModelPoMo' (the mutation
    // coefficients point to the rates of ModelDNA, the fixed state
    // frequencies to the state frequencies and so on).
    phylo_tree->aln->num_states = 4;
    dna_model = new ModelDNA(model_name, model_params, freq_type, freq_params, phylo_tree);
    phylo_tree->aln->num_states = num_states;
    num_params = dna_model->num_params + 1;
    
	this->name = dna_model->name + "+rP" + convertIntToString(N);
    this->full_name =
        "reversible PoMo with N=" +
        convertIntToString(N) + " and " +
        dna_model->full_name + " substitution model; " +
        convertIntToString(num_states) + " states in total";

    eps = 1e-6;

    // Mutation probabilities point to the rates of the DNA model.
    mutation_prob = dna_model->rates;
	for (int i = 0; i < 6; i++) mutation_prob[i] = 1e-5;

    // Frequencies of the boundary states (fixed states, e.g., 10A).
    // These correspond to the state frequencies in the DNA
    // substitution models.
    freq_fixed_states = dna_model->state_freq;

    // Create PoMo rate matrix.  This is the actual rate matrix of
    // PoMo.  TODO: Is the distinction rates[] and rate_matrix[]
    // really necessary?
	rate_matrix = new double[num_states*num_states];

    // Check state frequencies.
    unsigned int abs_state_freq[num_states];
    phylo_tree->aln->computeAbsoluteStateFreq(abs_state_freq);
    if (verbose_mode >= VB_MAX) {
        std::cout << "Absolute empirical state frequencies:" << std::endl;
        for (int i = 0; i < num_states; i++)
            std::cout << abs_state_freq[i] << " ";
        std::cout << std::endl;
    }
    
    freq_type = dna_model->freq_type;
    // ModelGTR.freq_type is not set correctly.  Set it here
    // explicitely.  Needed for some verbose output functions.
    this->freq_type = dna_model->freq_type;
    
    switch (freq_type) {
    case FREQ_EQUAL:            // '+FQ'
    case FREQ_ESTIMATE:         // '+FO'
        for (int i = 0; i < 4; i++) freq_fixed_states[i] = 1.0;
        break;
    case FREQ_EMPIRICAL:        // '+F'
        // Get the fixed state frequencies from the data and normalize
        // them such that the last one is 1.0.
        estimateEmpiricalFixedStateFreqs(abs_state_freq,
                                         freq_fixed_states);
        for (int i = 0; i < nnuc; i++)
            freq_fixed_states[i] /= freq_fixed_states[3];
        // Set highest_freq_state.
		for (int i = 0; i < num_states; i++)
			if (abs_state_freq[i] > abs_state_freq[highest_freq_state])
				highest_freq_state = i;
        break;
    case FREQ_USER_DEFINED:     // '+FU'
        // ModelDNA should have set them already.
        if (freq_fixed_states[0] == 0.0)
            outError("State frequencies not specified");
        break;
    case FREQ_UNKNOWN:
        outError("No frequency type given.");
        break;
    default:
        outError("Unknown frequency type.");
        break;
    }

	updatePoMoStatesAndRates();

	decomposeRateMatrix();
	if (verbose_mode >= VB_MAX)
		writeInfo(cout);
}

ModelPoMo::~ModelPoMo() {
	delete [] rate_matrix;
//	delete [] freq_fixed_states;
    delete dna_model;
//	delete [] mutation_prob;
}

double ModelPoMo::computeNormConst() {
	int i, j;
	double harmonic = 0.0;
	for (i = 1; i < N; i++)
		harmonic += 1.0/(double)i;

	double norm_fixed = 0.0, norm_polymorphic = 0.0;
    // // Tue Mar 17 14:29:37 CET 2015; Set the sum over the fixed
    // // frequencies to 1.0 so that they can be compared with the
    // // frequencies from the GTR model.
    // norm_fixed = 1.0;
	for (i = 0; i < 4; i++)
		norm_fixed += freq_fixed_states[i];
    // TODO: This could be made faster (second sum over j < i only).
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++)
			if (i != j)
				norm_polymorphic +=
                    freq_fixed_states[i] * freq_fixed_states[j] * mutCoeff(i, j);
	}
	norm_polymorphic *= N * harmonic;
	return 1.0/(norm_fixed + norm_polymorphic);
}

// void ModelPoMo::updateFreqFixedState () {
//     // Sat Mar 28 22:10:49 CET 2015: This function is not needed, when
//     // the frequencies of the fixed states do not sum up to 1.0.  This
//     // might be better, because of numerical instabilities.
//     double f_sum = freq_fixed_states[0] +
//         freq_fixed_states[1] + freq_fixed_states[2];
//     // Make sure that this assertion is met and that IQ-Tree is
//     // not unstable.  Probably the lh diverges and this assertion is
//     // not met sometimes?
//     assert(f_sum <= 1.0);
//     freq_fixed_states[3] = 1.0 - f_sum;
// }

void ModelPoMo::computeStateFreq () {
	double norm = computeNormConst();
	int state;

	for (state = 0; state < num_states; state++) {
		if (isFixed(state))
			state_freq[state] = freq_fixed_states[state]*norm;
		else {
			int k, X, Y;
			decomposeState(state, k, X, Y);
			state_freq[state] =
                norm * freq_fixed_states[X] * freq_fixed_states[Y] *
                mutCoeff(X, Y)*N*N / (k*(N-k));
		}
    }
}

void ModelPoMo::updatePoMoStatesAndRates () {
	int state1, state2;

    // Activate this if frequencies of fixed states sum up to 1.0.
    // updateFreqFixedState();
	computeStateFreq();

	// Loop over rows (transition starting from state1).
	for (state1 = 0; state1 < num_states; state1++) {
		double row_sum = 0.0;
		// Loop over columns in row state1 (transition to state2).
		for (state2 = 0; state2 < num_states; state2++)
			if (state2 != state1) {
				row_sum +=
                    (rate_matrix[state1*num_states+state2] =
                     computeProbBoundaryMutation(state1, state2));
			}
		rate_matrix[state1*num_states+state1] = -(row_sum);
	}
    if (verbose_mode >= VB_MAX) {
        std::cout << std::setprecision(7)
                  << "DEBUG: Rate Matrix calculated." << std::endl
                  << "DEBUG: mu=" << "\t"
                  << mutation_prob[0] << "\t"
                  << mutation_prob[1] << "\t"
                  << mutation_prob[2] << "\t"
                  << mutation_prob[3] << "\t"
                  << mutation_prob[4] << "\t"
                  << mutation_prob[5] << std::endl;
        std::cout << "DEBUG: " << std::setprecision(3) << "PIs:\t"
                  << freq_fixed_states[0] << "\t"
                  << freq_fixed_states[1] << "\t"
                  << freq_fixed_states[2] << "\t"
                  << freq_fixed_states[3] << std::endl;
    }
}

// void ModelPoMo::initMoranWithMutation() {

// 	// // This code was used to run a dummy JC69 model with 58 states.
// 	// int i=0;

// 	// for (i=0; i<num_states*(num_states-1)/2; i++) {
// 	// 	rates[i] = 1.0;
// 	// }
// 	// for (i=0; i<num_states; i++) {
// 	// 	state_freq[i] = 1.0/num_states;
// 	// }

// 	// Recheck this.
// 	// Initialize rate matrix Q[state1,state2] = transition rate from
// 	// state1 to state2.
// 	int state1, state2;
// 	if (verbose_mode >= VB_MED) cout << "PoMo rate matrix:" << endl;
// 	// Loop over rows (transition starting from state1)
// 	for (state1 = 0; state1 < num_states; state1++) {
// 		double row_sum = 0.0;
// 		// Loop over columns (transition to state2)
// 		for (state2 = 0; state2 < num_states; state2++) {
// 			if (state1 == state2) {
// 				// Q = P - I
// 				rate_matrix[state1*num_states+state2] = computeProb(state1, state2) - 1.0;
// 			} else {
// 				rate_matrix[state1*num_states+state2] = computeProb(state1, state2);
// 			}
// 			// Compute row sum.
// 			row_sum += rate_matrix[state1*num_states+state2];
// 			if (verbose_mode >= VB_MED)
// 				cout << rate_matrix[state1*num_states+state2] << "\t";
// 		}
// 		if (verbose_mode >= VB_MED) cout << endl;
// 		// Check row sum.
// 		if (fabs(row_sum) > 0.000001) outError("Row sum not equal 0");
// 	}

// }

// double ModelPoMo::computeP(int i, int major, int minor) {
// 	// Cf. Moran model with mutation.
// 	int N = phylo_tree->aln->virtual_pop_size;
// 	return (1.0 - mutation_prob[major*4+minor])*(i)*(N-i)/(N*N) +
// 			mutation_prob[minor*4+major]*(N-i)*(N-i)/(N*N);
// }

// double ModelPoMo::computeR(int i, int major, int minor) {
// 	// Cf. Moran model with mutation.
// 	int N = phylo_tree->aln->virtual_pop_size;
// 	return (mutation_prob[major*4+minor]+mutation_prob[minor*4+major])*i*(N-i)/(N*N) +
// 			(1-mutation_prob[minor*4+major])*(N-i)*(N-i)/(N*N) +
// 			(1-mutation_prob[major*4+minor])*i*i/(N*N);
// }

void ModelPoMo::decomposeState(int state, int &i, int &nt1, int &nt2) {
	if (state < 4) {
		// Fixed A, C, G or T
		i = N;
		nt1 = state;
		nt2 = -1; // -1 for unknown nt
	} else if (state < 4+(N-1)) {
		// (iA,N-iC)
		i = state-3;
		nt1 = 0; // A
		nt2 = 1; // C
	} else if (state < 4+2*(N-1)) {
		// (iA,N-iG)
		i = state-3-(N-1);
		nt1 = 0; // A
		nt2 = 2; // G
	} else if (state < 4+3*(N-1)) {
		// (iA,N-iT)
		i = state-3-2*(N-1);
		nt1 = 0; // A
		nt2 = 3; // T
	} else if (state < 4+4*(N-1)) {
		// (iC,N-iG)
		i = state-3-3*(N-1);
		nt1 = 1; // C
		nt2 = 2; // G
	} else if (state < 4+5*(N-1)) {
		// (iC,N-iT)
		i = state-3-4*(N-1);
		nt1 = 1; // C
		nt2 = 3; // T
	} else if (state < 4+6*(N-1)) {
		// (iG,N-iT)
		i = state-3-5*(N-1);
		nt1 = 2; // G
		nt2 = 3; // T
	} else {
		outError("State exceeds limit");
	}
}

// double ModelPoMo::computeProb(int state1, int state2) {
// 	int N = phylo_tree->aln->virtual_pop_size;

// 	// Both states are decomposed into the abundance of the first
// 	// allele as well as the nucleotide of the first and the second
// 	// allele.
// 	int i1=0, i2=0, nt1=-1, nt2=-1, nt3=-1, nt4=-1;
// 	decomposeState(state1, i1, nt1, nt2);
// 	decomposeState(state2, i2, nt3, nt4);
// 	// Either the first nucleotides match or the first of state 1 with
// 	// the second of state 2 or the first of state 2 with the second
// 	// of state 1.  Additionally, we have to consider fixed states as
// 	// special cases.
// 	if (nt1 == nt3 && (nt2==nt4 || nt2==-1 || nt4 == -1)) {
// 		if (i1==i2) {
// 			if (i1==N) {
// 				// e.g.: 10A -> 10A
// 				double sum = 0;
// 				for (int nt=0; nt < 4; nt++)
// 					if (nt != nt1) sum += computeR(N, nt1, nt);
// 				return sum-2.0;
// 			} else
// 				// e.g.: nA(N-n)C -> nA(N-n)C where 0<n<N
// 				return computeR(i1, nt1, nt2);
// 		} else if (i1+1==i2)
// 			// e.g.: 2A8C -> 3A7C or 9A1C -> 10A
// 			if (nt2 == -1)
// 				// e.g. 10A ->
// 				return computeP(i1, nt1, nt4);
// 			else
// 				return computeP(i1, nt1, nt2);
// 		else if (i1-1 == i2)
// 			// e.g.: 3A7C -> 2A8C or 10A -> 9A1C
// 			if (nt2 == -1)
// 				// e.g. 10A -> 9A1C
// 				return computeP(N-i1, nt4, nt1);
// 			else
// 				// e.g. 9A1C -> 8A2C
// 				return computeP(N-i1, nt2, nt1);
// 		else
// 			// 0 for all others
// 			return 0.0;
// 	} else if (nt1 == nt4 && nt2 == -1 && i2 == 1)  {
// 		// e.g.: 10G -> 1A9G
// 		return computeP(0, nt3, nt1);
// 	} else if (nt2 == nt3  && i1 == 1 && nt4 == -1) {
// 		// E.g.: 1A9G -> 10G
// 		return computeP(N-1, nt2, nt1);
// 	} else
// 		// 0 for all other transitions
// 		return 0.0;
// }

bool ModelPoMo::isFixed(int state) {
	return (state < 4);
}

bool ModelPoMo::isPolymorphic(int state) {
	return (!isFixed(state));
}

double ModelPoMo::mutCoeff(int nt1, int nt2) {
	assert(nt1!=nt2 && nt1<4 && nt2<4);
	if (nt2 < nt1) {
		int tmp=nt1;
		nt1=nt2;
		nt2=tmp;
	}
	if (nt1==0) return mutation_prob[nt2-1];
	if (nt1==1) return mutation_prob[nt2+1];
	if (nt1==2) return mutation_prob[5];
	assert(0);
}

double ModelPoMo::computeProbBoundaryMutation(int state1, int state2) {
    // The transition rate to the same state will be calculated by
    // (row_sum = 0).
	assert(state1 != state2);

	// Both states are decomposed into the abundance of the first
	// allele as well as the nucleotide of the first and the second
	// allele.
	int i1=0, i2=0, nt1=-1, nt2=-1, nt3=-1, nt4=-1;
	decomposeState(state1, i1, nt1, nt2);
	decomposeState(state2, i2, nt3, nt4);

	// Either the first nucleotides match or the first of state 1 with
	// the second of state 2 or the first of state 2 with the second
	// of state 1.  Additionally, we have to consider fixed states as
	// special cases.
	if (nt1 == nt3 && (nt2==nt4 || nt2==-1 || nt4 == -1)) {
		assert(i1 != i2); // because state1 != state2
		if (i1+1==i2)
			// e.g.: 2A8C -> 3A7C or 9A1C -> 10A
            return double(i1*(N-i1)) / double(N*N);
		else if (i1-1 == i2)
			// e.g.: 3A7C -> 2A8C or 10A -> 9A1C
			if (nt2 == -1)
				// e.g. 10A -> 9A1C
				// return mutCoeff(nt1,nt4) * state_freq[nt4];
				return mutCoeff(nt1,nt4) * freq_fixed_states[nt4];
			else
				// e.g. 9A1C -> 8A2C
				return double(i1*(N-i1)) / double(N*N);
		else
			return 0.0;
	} else if (nt1 == nt4 && nt2 == -1 && i2 == 1)  {
		// e.g.: 10G -> 1A9G
		//return mutCoeff(nt1,nt3) * state_freq[nt3];
		return mutCoeff(nt1,nt3) * freq_fixed_states[nt3];
	} else if (nt2 == nt3  && i1 == 1 && nt4 == -1) {
		// E.g.: 1A9G -> 10G
		return double(i1*(N-i1)) / double(N*N);
	} else
		// 0 for all other transitions
		return 0.0;
}

int ModelPoMo::getNDim() {
 	return dna_model->getNDim()+1;
}

void ModelPoMo::setBounds(double *lower_bound,
                          double *upper_bound,
                          bool *bound_check) {
	int i, ndim = getNDim();
    
    // Mutation rates.
	for (i = 1; i <= ndim; i++) {
		lower_bound[i] = POMO_MIN_RATE;
		upper_bound[i] = POMO_MAX_RATE;
		bound_check[i] = false;
	}

    // Frequencies of fixed states.
    if (freq_type == FREQ_ESTIMATE) {
        for (i = ndim-nnuc+2; i <= ndim; i++) {
            lower_bound[i] = POMO_MIN_REL_FREQ * freq_fixed_states[3];
            upper_bound[i] = POMO_MAX_REL_FREQ * freq_fixed_states[3];
            bound_check[i] = false;
        }        
    }
}

void ModelPoMo::setVariables(double *variables) {
	// for (int i = 1; i <= 6; i++) {
	// 	variables[i] = mutation_prob[i-1];
	// }
	// for (int i = 7; i <= 9; i++) {
	// 	variables[i] = freq_fixed_states[i-7];
	// }
	if (num_params > 0) {
		int num_all = dna_model->param_spec.length();
		for (int i = 0; i < num_all; i++)
            variables[(int)dna_model->param_spec[i]+1] = mutation_prob[i];
	}
	if (freq_type == FREQ_ESTIMATE) {
		int ndim = getNDim();
		memcpy(variables+(ndim-nnuc+2), freq_fixed_states, (nnuc-1)*sizeof(double));
	}
}

void ModelPoMo::getVariables(double *variables) {
	int i;
	// for (i = 1; i <= 6; i++) {
	// 	mutation_prob[i-1] = variables[i];
	// }
	// for (i = 7; i <= 9; i++) {
	// 	freq_fixed_states[i-7] = variables[i];
	// }
	// updatePoMoStatesAndRates();

	if (num_params > 0) {
		int num_all = dna_model->param_spec.length();
		if (verbose_mode >= VB_MAX) {
			for (i = 1; i <= num_params; i++) {
                cout << setprecision(8);
				cout << "  Estimated mutation probabilities[" << i << "] = ";
                cout << variables[i] << endl;
            }
		}
		for (i = 0; i < num_all; i++)
            mutation_prob[i] = variables[(int)dna_model->param_spec[i]+1];
	}
	if (freq_type == FREQ_ESTIMATE) {
		int ndim = getNDim();
		memcpy(freq_fixed_states, variables+(ndim-nnuc+2), (nnuc-1)*sizeof(double));
		if (verbose_mode >= VB_MAX) {
			for (i = 0; i < nnuc-1; i++) {
                cout << setprecision(8);
				cout << "  Estimated fixed frequencies[" << i << "] = ";
                cout << variables[ndim-nnuc+2+i] << endl;
            }
		}
		// double sum = 0;
		// for (i = 0; i < num_states-1; i++) 
		// 	sum += state_freq[i];
		// state_freq[num_states-1] = 1.0 - sum;
	}
    updatePoMoStatesAndRates();
}

void ModelPoMo::writeInfo(ostream &out) {
	int i;
    int state1;
    ios  state(NULL);
    state.copyfmt(out);

    out << setprecision(8);
    out << endl;

    out << "==========================" << endl;
    out << "Frequency of fixed states: " << endl;;
	for (i = 0; i < 4; i++)
		out << freq_fixed_states[i] << " ";
	out << endl << endl;

    out << "===============" << endl;
    out << "Mutation rates: " << endl;
	for (i = 0; i < 6; i++)
		out << mutation_prob[i] << " ";
	out << endl << endl;;

    out << "==================================" << endl;
    out << "State frequency vector state_freq: " << endl;
    for (state1 = 0; state1 < num_states; state1++) {
        if (state1 == 4 || (state1-4)%(N-1) == 0) out << endl;
        out << state_freq[state1] << " ";
    }
    out << endl << endl;

    // out << "Rates (upper triangular) without diagonal: ";
    // i = 0;
    // for (state1 = 0; state1 < num_states; state1++) {
    //     for (state2 = state1+1; state2 < num_states; state2++) {
    //         out << rates[i++] << '\t';
    //     }
    //     out << endl;
    // }

    // out << "PoMo rate matrix:" << endl;
    // for (int state1 = 0; state1 < num_states; state1++) {
    //     for (int state2 = 0; state2 < num_states; state2++)
    //         out << rate_matrix[state1*num_states+state2] << "\t";
    //     out << endl;
    // }

    out.copyfmt(state);
}

void ModelPoMo::computeRateMatrix(double **r_matrix, double *s_freqs, int n_states) {
    // // Normalize the rate matrix such that on average one mutation
    // // event happens per delta_t = 1.0.
    // double sum = 0.0;
    // for (int i = 0; i < 4; i++) {
    //     sum -= s_freqs[i]*rate_matrix[i*n_states + i];
    // }

    // Normalzie the rate matrix such that on average one event
    // happens per delta_t = 1.0.  This seems to be more stable.
    double tot_sum = 0.0;
    double row_sum;
    
    for (int i = 0; i < n_states; i++) {
        row_sum = 0.0;
        for (int j = 0; j < n_states; j++) {
            if (i != j) row_sum += rate_matrix[i*n_states + j];
        }
        tot_sum += s_freqs[i]*row_sum;
    }
    
    for (int i = 0; i < n_states; i++) {
        for (int j = 0; j < n_states; j++) {
            r_matrix[i][j] = rate_matrix[i*n_states+j] / tot_sum;
        }
    }

    // std::cout << "DEBUG Rate Matrix." << std::endl;
    // for (int i = 0; i < n_states; i++) {
    //     std::cout << "Row " << i << ": ";
    //     for (int j = 0; j < n_states; j++) {
    //         std::cout << rate_matrix[i*n_states+j] << " ";
    //     }
    //     std::cout << std::endl;
    // }
    // std::cout << "DEBUG State Frequency." << setprecision(10) << std::endl;
    // for (int i = 0; i < n_states; i++) {
    //     std::cout << s_freqs[i] << " ";
    // }
    // std::cout << std::endl;
}

double ModelPoMo::targetFunk(double x[]) {
    // Define PoMo targetFunkt because state_freq might be very low.
	getVariables(x);
	// if (state_freq[num_states-1] < 1e-4) return 1.0e+12;
	decomposeRateMatrix();
	assert(phylo_tree);
	phylo_tree->clearAllPartialLH();
	return -phylo_tree->computeLikelihood();
}

bool ModelPoMo::isUnstableParameters() {
    // More checking could be done.
    for (int i = 0; i < num_states; i++) {
        if (state_freq[i] < eps) return true;
    }
    return false;
}

// bool ModelPoMo::setRateType(const char *rate_str) {
// 	int num_ch = strlen(rate_str);
// 	int i;

// 	if (num_ch != getNumRateEntries()) {
// 		return false;
// 	}

// 	// Only accept string of digits.
// 	for (i = 0; i < num_ch; i++)
// 		if (!isdigit(rate_str[i])) return false;

// 	map<char,char> param_k;
// 	num_params = 0;
// 	param_spec = "";
// 	// Set ID of last element to 0.
// 	param_k[rate_str[num_ch-1]] = 0;
// 	for (i = 0; i < num_ch; i++) {
// 		if (param_k.find(rate_str[i]) == param_k.end()) {
// 			num_params++;
// 			param_k[rate_str[i]] = (char)num_params;
// 			param_spec.push_back(num_params);
// 		} else {
// 			param_spec.push_back(param_k[rate_str[i]]);
// 		}
// 	}

//     bool t = (param_spec.length() == (unsigned int) num_ch);
// 	assert(t);

//     // Do not normalize mutation_prob for PoMo.

// 	// double *avg_rates = new double[num_params+1];
// 	// int *num_rates = new int[num_params+1];
// 	// memset(avg_rates, 0, sizeof(double) * (num_params+1));
// 	// memset(num_rates, 0, sizeof(int) * (num_params+1));
// 	// for (i = 0; i < param_spec.size(); i++) {
// 	// 	avg_rates[(int)param_spec[i]] += mutation_prob[i];
// 	// 	num_rates[(int)param_spec[i]]++;
// 	// }
// 	// for (i = 0; i <= num_params; i++)
// 	// 	avg_rates[i] /= num_rates[i];
// 	// for (i = 0; i < param_spec.size(); i++) {
// 	// 	mutation_prob[i] = avg_rates[(int)param_spec[i]] / avg_rates[0];
// 	// }
// 	// if (verbose_mode >= VB_DEBUG) {
// 	// 	cout << "Initialized mutation rates: ";
// 	// 	for (i = 0; i < param_spec.size(); i++)
// 	// 		cout << mutation_prob[i] << " ";
// 	// 	cout << endl;
// 	// }
// 	// delete [] num_rates;
// 	// delete [] avg_rates;

// 	param_fixed.resize(num_params+1, false);
//     // Fix the last entry.
// 	param_fixed[0] = true;
// 	return true;
// }

// void ModelPoMo::readFixedStateFreq(istream &in) {
// 	int i;
// 	for (i = 0; i < nnuc; i++) {
// 		if (!(in >> freq_fixed_states[i])) 
// 			throw "State frequencies could not be read";
// 		if (freq_fixed_states[i] < 0.0)
// 			throw "Negative state frequencies found";
// 	}
// 	double sum = 0.0;
// 	for (i = 0; i < nnuc; i++) sum += freq_fixed_states[i];
// 	if (fabs(sum-1.0) > 1e-3)
// 		throw "State frequencies do not sum up to 1.0";
// }

// void ModelPoMo::readFixedStateFreq(string str) {
// 	int i;
// 	unsigned int end_pos = 0;
// 	for (i = 0; i < nnuc; i++) {
// 		int new_end_pos;
// 		freq_fixed_states[i] = convert_double(str.substr(end_pos).c_str(), new_end_pos);
// 		end_pos += new_end_pos;
// 		//cout << i << " " << freq_fixed_states[i] << endl;
// 		if (freq_fixed_states[i] < 0.0 || freq_fixed_states[i] > 1)
// 			outError("State frequency must be in [0,1] in ", str);
// 		if (i == nnuc-1 && end_pos < str.length())
// 			outError("Unexpected end of string ", str);
// 		if (end_pos < str.length() && str[end_pos] != ',')
// 			outError("Comma to separate state frequencies not found in ", str);
// 		end_pos++;
// 	}
// 	double sum = 0.0;
// 	for (i = 0; i < nnuc; i++) sum += freq_fixed_states[i];
// 	if (fabs(sum-1.0) > 1e-2)
// 		outError("State frequencies do not sum up to 1.0 in ", str);
// }

// void ModelPoMo::readMutationParameters(const char *file_name) { 
// 	try {
// 		ifstream in(file_name);
// 		if (in.fail())
// 			outError("Invalid model name ", file_name);
// 		cout << "Reading model parameters from file " << file_name << endl;
// 		readMutationRates(in);
// 		readFixedStateFreq(in);
// 		in.close();
// 	}
// 	catch (const char *str) {
// 		outError(str);
// 	} 
// 	num_params = 0;
// 	writeInfo(cout);
// }

// void ModelPoMo::readMutationRates(istream &in) {
// 	int nrates = getNumRateEntries();
// 	string str;
// 	in >> str;
// 	if (str == "equalrate") {
// 		for (int i = 0; i < nrates; i++)
// 			mutation_prob[i] = 1.0;
// 	} else {
// 		try {
// 			mutation_prob[0] = convert_double(str.c_str());
// 		} catch (string &str) {
// 			outError(str);
// 		}
// 		if (mutation_prob[0] < 0.0)
// 			throw "Negative rates not allowed";
// 		for (int i = 1; i < nrates; i++) {
// 			if (!(in >> mutation_prob[i]))
// 				throw "Rate entries could not be read";
// 			if (mutation_prob[i] < 0.0)
// 				throw "Negative rates not allowed";
// 		}
// 	}
// }

// void ModelPoMo::readMutationRates(string str) {
// 	unsigned int nrates = *max_element(param_spec.begin(), param_spec.end());
// 	unsigned int end_pos = 0;
// 	unsigned int i, j;
// 	for (j = 0; j < param_spec.length(); j++)
// 		mutation_prob[j] = 1.0;
// 	num_params = 0;
// 	for (i = 0; i < nrates && end_pos < str.length(); i++) {
// 		int new_end_pos;
// 		double rate = 0;
// 		if (str[end_pos] == '?') {
// 			param_fixed[i+1] = false;
// 			end_pos++;
// 			rate = i + 0.4;
// 			num_params++;
// 		} else {
// 			param_fixed[i+1] = true;
// 			try {
// 				rate = convert_double(str.substr(end_pos).c_str(), new_end_pos);
// 			} catch (string str) {
// 				outError(str);
// 			}
// 			end_pos += new_end_pos;
// 		}
// 		if (rate < 0.0)
// 			outError("Negative rates found");
// 		if (i == nrates-1 && end_pos < str.length())
// 			outError("String too long ", str);
// 		if (i < nrates-1 && end_pos >= str.length())
// 			outError("Unexpected end of string ", str);
// 		if (end_pos < str.length() && str[end_pos] != ',')
// 			outError("Comma to separate rates not found in ", str);
// 		end_pos++;
// 		for (j = 0; j < param_spec.length(); j++)
// 			if (param_spec[j] == (int) i+1)
// 				mutation_prob[j] = rate;
// 	}
// }

void
ModelPoMo::estimateEmpiricalFixedStateFreqs(unsigned int * abs_state_freq,
                                            double * freq_fixed_states)
{
    int n;
    int x;
    int y;

    int sum[nnuc];
    int tot_sum = 0;
    memset (sum, 0, nnuc * sizeof(int));
    
    for (int i = 0; i < num_states; i++) {
        decomposeState(i, n, x, y);
        sum[x]+= n*abs_state_freq[i];
        if (y >= 0) sum[y]+= (N-n)*abs_state_freq[i];
    }
    for (int i = 0; i < nnuc; i++) {
        tot_sum += sum[i];
    }
    for (int i = 0; i < nnuc; i++) {
        freq_fixed_states[i] = (double) sum[i]/tot_sum;
    }
}

void ModelPoMo::reportPoMoRates(ofstream &out) {
    out << setprecision(8);
    out << "Estimated mutation rates:" << endl;
	for (int i = 0; i < 6; i++)
		out << mutation_prob[i] << " ";
	out << endl << endl;;

}

void ModelPoMo::reportPoMoStateFreqs(ofstream &out) {
    double sum = 0.0;
    for (int i = 0; i < nnuc; i++) {
        sum += freq_fixed_states[i];
    }

    out << setprecision(8);
    out << "(Estimated) frequencies of fixed states:" << endl;;
	for (int i = 0; i < nnuc; i++)
		out << freq_fixed_states[i]/sum << " ";
	out << endl << endl;

}
