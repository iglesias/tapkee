/**
 * Copyright (c) 2013, Laurens van der Maaten (Delft University of Technology)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    This product includes software developed by the Delft University of Technology.
 * 4. Neither the name of the Delft University of Technology nor the names of
 *    its contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY LAURENS VAN DER MAATEN ''AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL LAURENS VAN DER MAATEN BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 */
#pragma once

/* Tapkee includes */
#include <tapkee/external/barnes_hut_sne/quadtree.hpp>
#include <tapkee/external/barnes_hut_sne/vptree.hpp>
#include <tapkee/utils/logging.hpp>
#include <tapkee/utils/time.hpp>
/* End of Tapkee includes */

#include <cstring>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

//! Namespace containing implementation of t-SNE algorithm
namespace tsne
{

using tapkee::ScalarType;

static inline ScalarType sign(ScalarType x)
{
    return (x == .0 ? .0 : (x < .0 ? -1.0 : 1.0));
}

class TSNE
{
  public:
    void run(tapkee::DenseMatrix& X, int N, int D, ScalarType* Y, int no_dims, ScalarType perplexity, ScalarType theta)
    {
        // Determine whether we are using an exact algorithm
        bool exact = (theta == .0) ? true : false;
        if (exact)
            tapkee::Logging::instance().message_info("Using exact t-SNE algorithm");
        else
            tapkee::Logging::instance().message_info("Using Barnes-Hut-SNE algorithm");

        // Set learning parameters
        int max_iter = 1000, stop_lying_iter = 250, mom_switch_iter = 250;
        ScalarType momentum = .5, final_momentum = .8;
        ScalarType eta = 200.0;

        tapkee::DenseMatrix dY(N, no_dims);
        tapkee::DenseMatrix uY(N, no_dims);
        tapkee::DenseMatrix gains(N, no_dims);
        tapkee::DenseMatrix P;
        uY.setZero();
        gains.setConstant(1.0);

        // Normalize input data (to prevent numerical problems)
        int* row_P = NULL;
        int* col_P = NULL;
        ScalarType* val_P = NULL;
        {
            tapkee::tapkee_internal::timed_context context("Input similarities computation");
            zeroMean(X.data(), N, D);
            X.array() /= X.maxCoeff();

            // Compute input similarities for exact t-SNE
            if (exact)
            {

                // Compute similarities
                P = tapkee::DenseMatrix(N, N);
                computeGaussianPerplexity(X.data(), N, D, P.data(), perplexity);

                // Symmetrize input similarities
                for (int n = 0; n < N; n++)
                {
                    for (int m = n + 1; m < N; m++)
                    {
                        P.data()[n * N + m] += P.data()[m * N + n];
                        P.data()[m * N + n] = P.data()[n * N + m];
                    }
                }
                P.array() /= P.array().sum();
            }

            // Compute input similarities for approximate t-SNE
            else
            {

                // Compute asymmetric pairwise input similarities
                computeGaussianPerplexity(X.data(), N, D, &row_P, &col_P, &val_P, perplexity, (int)(3 * perplexity));

                // Symmetrize input similarities
                symmetrizeMatrix(&row_P, &col_P, &val_P, N);
                ScalarType sum_P = .0;
                for (int i = 0; i < row_P[N]; i++)
                    sum_P += val_P[i];
                for (int i = 0; i < row_P[N]; i++)
                    val_P[i] /= sum_P;
            }

            // Lie about the P-values
            if (exact)
                P.array() *= 12.0;
            else
            {
                for (int i = 0; i < row_P[N]; i++)
                    val_P[i] *= 12.0;
            }

            // Initialize solution (randomly)
            for (int i = 0; i < N * no_dims; i++)
                Y[i] = tapkee::gaussian_random() * .0001;
        }

        {
            tapkee::tapkee_internal::timed_context context("Main t-SNE loop");
            for (int iter = 0; iter < max_iter; iter++)
            {

                // Compute (approximate) gradient
                if (exact)
                    computeExactGradient(P.data(), Y, N, no_dims, dY.data());
                else
                    computeGradient(P.data(), row_P, col_P, val_P, Y, N, no_dims, dY.data(), theta);

                // Update gains
                for (int i = 0; i < N * no_dims; i++)
                    gains.data()[i] =
                        (sign(dY.data()[i]) != sign(uY.data()[i])) ? (gains.data()[i] + .2) : (gains.data()[i] * .8);
                for (int i = 0; i < N * no_dims; i++)
                    if (gains.data()[i] < .01)
                        gains.data()[i] = .01;

                // Perform gradient update (with momentum and gains)
                for (int i = 0; i < N * no_dims; i++)
                    uY.data()[i] = momentum * uY.data()[i] - eta * gains.data()[i] * dY.data()[i];
                for (int i = 0; i < N * no_dims; i++)
                    Y[i] = Y[i] + uY.data()[i];

                // Make solution zero-mean
                zeroMean(Y, N, no_dims);

                // Stop lying about the P-values after a while, and switch momentum
                if (iter == stop_lying_iter)
                {
                    if (exact)
                        P.array() /= 12.0;
                    else
                    {
                        for (int i = 0; i < row_P[N]; i++)
                            val_P[i] /= 12.0;
                    }
                }
                if (iter == mom_switch_iter)
                    momentum = final_momentum;

                // Print out progress
                if ((iter > 0) && ((iter % 50 == 0) || (iter == max_iter - 1)))
                {
                    ScalarType C = .0;
                    if (exact)
                        C = evaluateError(P.data(), Y, N);
                    else
                        C = evaluateError(row_P, col_P, val_P, Y, N, theta); // doing approximate computation here!
                    tapkee::Logging::instance().message_info(fmt::format("Iteration {}: error is {}", iter, C));
                }
            }
            // Clean up memory
            if (!exact)
            {
                free(row_P);
                row_P = NULL;
                free(col_P);
                col_P = NULL;
                free(val_P);
                val_P = NULL;
            }
        }
    }

    void symmetrizeMatrix(int** _row_P, int** _col_P, ScalarType** _val_P, int N)
    {
        // Get sparse matrix
        int* row_P = *_row_P;
        int* col_P = *_col_P;
        ScalarType* val_P = *_val_P;

        // Count number of elements and row counts of symmetric matrix
        int* row_counts = (int*)calloc(N, sizeof(int));
        if (row_counts == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        for (int n = 0; n < N; n++)
        {
            for (int i = row_P[n]; i < row_P[n + 1]; i++)
            {
                // Check whether element (col_P[i], n) is present
                bool present = false;
                for (int m = row_P[col_P[i]]; m < row_P[col_P[i] + 1]; m++)
                {
                    if (col_P[m] == n)
                        present = true;
                }
                if (present)
                    row_counts[n]++;
                else
                {
                    row_counts[n]++;
                    row_counts[col_P[i]]++;
                }
            }
        }
        int no_elem = 0;
        for (int n = 0; n < N; n++)
            no_elem += row_counts[n];

        // Allocate memory for symmetrized matrix
        int* sym_row_P = (int*)malloc((N + 1) * sizeof(int));
        int* sym_col_P = (int*)malloc(no_elem * sizeof(int));
        ScalarType* sym_val_P = (ScalarType*)malloc(no_elem * sizeof(ScalarType));
        if (sym_row_P == NULL || sym_col_P == NULL || sym_val_P == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }

        // Construct new row indices for symmetric matrix
        sym_row_P[0] = 0;
        for (int n = 0; n < N; n++)
            sym_row_P[n + 1] = sym_row_P[n] + row_counts[n];

        // Fill the result matrix
        int* offset = (int*)calloc(N, sizeof(int));
        if (offset == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        for (int n = 0; n < N; n++)
        {
            for (int i = row_P[n]; i < row_P[n + 1]; i++)
            { // considering element(n, col_P[i])

                // Check whether element (col_P[i], n) is present
                bool present = false;
                for (int m = row_P[col_P[i]]; m < row_P[col_P[i] + 1]; m++)
                {
                    if (col_P[m] == n)
                    {
                        present = true;
                        if (n <= col_P[i])
                        { // make sure we do not add elements twice
                            sym_col_P[sym_row_P[n] + offset[n]] = col_P[i];
                            sym_col_P[sym_row_P[col_P[i]] + offset[col_P[i]]] = n;
                            sym_val_P[sym_row_P[n] + offset[n]] = val_P[i] + val_P[m];
                            sym_val_P[sym_row_P[col_P[i]] + offset[col_P[i]]] = val_P[i] + val_P[m];
                        }
                    }
                }

                // If (col_P[i], n) is not present, there is no addition involved
                if (!present)
                {
                    sym_col_P[sym_row_P[n] + offset[n]] = col_P[i];
                    sym_col_P[sym_row_P[col_P[i]] + offset[col_P[i]]] = n;
                    sym_val_P[sym_row_P[n] + offset[n]] = val_P[i];
                    sym_val_P[sym_row_P[col_P[i]] + offset[col_P[i]]] = val_P[i];
                }

                // Update offsets
                if (!present || (present && n <= col_P[i]))
                {
                    offset[n]++;
                    if (col_P[i] != n)
                        offset[col_P[i]]++;
                }
            }
        }

        // Divide the result by two
        for (int i = 0; i < no_elem; i++)
            sym_val_P[i] /= 2.0;

        // Return symmetrized matrices
        free(*_row_P);
        *_row_P = sym_row_P;
        free(*_col_P);
        *_col_P = sym_col_P;
        free(*_val_P);
        *_val_P = sym_val_P;

        // Free up some memery
        free(offset);
        offset = NULL;
        free(row_counts);
        row_counts = NULL;
    }

  private:
    void computeGradient(ScalarType* /*P*/, int* inp_row_P, int* inp_col_P, ScalarType* inp_val_P, ScalarType* Y, int N,
                         int D, ScalarType* dC, ScalarType theta)
    {
        // Construct quadtree on current map
        QuadTree* tree = new QuadTree(Y, N);

        // Compute all terms required for t-SNE gradient
        ScalarType sum_Q = .0;
        ScalarType* pos_f = (ScalarType*)calloc(static_cast<size_t>(N) * D, sizeof(ScalarType));
        ScalarType* neg_f = (ScalarType*)calloc(static_cast<size_t>(N) * D, sizeof(ScalarType));
        if (pos_f == NULL || neg_f == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        tree->computeEdgeForces(inp_row_P, inp_col_P, inp_val_P, N, pos_f);
        for (int n = 0; n < N; n++)
            tree->computeNonEdgeForces(n, theta, neg_f + n * D, &sum_Q);

        // Compute final t-SNE gradient
        for (int i = 0; i < N * D; i++)
        {
            dC[i] = pos_f[i] - (neg_f[i] / sum_Q);
        }
        free(pos_f);
        free(neg_f);
        delete tree;
    }

    void computeExactGradient(ScalarType* P, ScalarType* Y, int N, int D, ScalarType* dC)
    {
        // Make sure the current gradient contains zeros
        for (int i = 0; i < N * D; i++)
            dC[i] = 0.0;

        // Compute the squared Euclidean distance matrix
        ScalarType* DD = (ScalarType*)malloc(static_cast<size_t>(N) * N * sizeof(ScalarType));
        if (DD == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        computeSquaredEuclideanDistance(Y, N, D, DD);

        // Compute Q-matrix and normalization sum
        ScalarType* Q = (ScalarType*)malloc(static_cast<size_t>(N) * N * sizeof(ScalarType));
        if (Q == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        ScalarType sum_Q = .0;
        for (int n = 0; n < N; n++)
        {
            for (int m = 0; m < N; m++)
            {
                if (n != m)
                {
                    Q[n * N + m] = 1 / (1 + DD[n * N + m]);
                    sum_Q += Q[n * N + m];
                }
            }
        }

        // Perform the computation of the gradient
        for (int n = 0; n < N; n++)
        {
            for (int m = 0; m < N; m++)
            {
                if (n != m)
                {
                    ScalarType mult = (P[n * N + m] - (Q[n * N + m] / sum_Q)) * Q[n * N + m];
                    for (int d = 0; d < D; d++)
                    {
                        dC[n * D + d] += (Y[n * D + d] - Y[m * D + d]) * mult;
                    }
                }
            }
        }

        // Free memory
        free(DD);
        DD = NULL;
        free(Q);
        Q = NULL;
    }

    ScalarType evaluateError(ScalarType* P, ScalarType* Y, int N)
    {
        // Compute the squared Euclidean distance matrix
        ScalarType* DD = (ScalarType*)malloc(static_cast<size_t>(N) * N * sizeof(ScalarType));
        ScalarType* Q = (ScalarType*)malloc(static_cast<size_t>(N) * N * sizeof(ScalarType));
        if (DD == NULL || Q == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        computeSquaredEuclideanDistance(Y, N, 2, DD);

        // Compute Q-matrix and normalization sum
        ScalarType sum_Q = DBL_MIN;
        for (int n = 0; n < N; n++)
        {
            for (int m = 0; m < N; m++)
            {
                if (n != m)
                {
                    Q[n * N + m] = 1 / (1 + DD[n * N + m]);
                    sum_Q += Q[n * N + m];
                }
                else
                    Q[n * N + m] = DBL_MIN;
            }
        }
        for (int i = 0; i < N * N; i++)
            Q[i] /= sum_Q;

        // Sum t-SNE error
        ScalarType C = .0;
        for (int n = 0; n < N; n++)
        {
            for (int m = 0; m < N; m++)
            {
                C += P[n * N + m] * log((P[n * N + m] + 1e-9) / (Q[n * N + m] + 1e-9));
            }
        }

        // Clean up memory
        free(DD);
        free(Q);
        return C;
    }

    ScalarType evaluateError(int* row_P, int* col_P, ScalarType* val_P, ScalarType* Y, int N, ScalarType theta)
    {
        // Get estimate of normalization term
        const int QT_NO_DIMS = 2;
        QuadTree* tree = new QuadTree(Y, N);
        ScalarType buff[QT_NO_DIMS] = {.0, .0};
        ScalarType sum_Q = .0;
        for (int n = 0; n < N; n++)
            tree->computeNonEdgeForces(n, theta, buff, &sum_Q);
        delete tree;

        // Loop over all edges to compute t-SNE error
        int ind1, ind2;
        ScalarType C = .0, Q;
        for (int n = 0; n < N; n++)
        {
            ind1 = n * QT_NO_DIMS;
            for (int i = row_P[n]; i < row_P[n + 1]; i++)
            {
                Q = .0;
                ind2 = col_P[i] * QT_NO_DIMS;
                for (int d = 0; d < QT_NO_DIMS; d++)
                    buff[d] = Y[ind1 + d];
                for (int d = 0; d < QT_NO_DIMS; d++)
                    buff[d] -= Y[ind2 + d];
                for (int d = 0; d < QT_NO_DIMS; d++)
                    Q += buff[d] * buff[d];
                Q = (1.0 / (1.0 + Q)) / sum_Q;
                C += val_P[i] * log((val_P[i] + FLT_MIN) / (Q + FLT_MIN));
            }
        }
        return C;
    }

    void zeroMean(ScalarType* X, int N, int D)
    {
        // Compute data mean
        ScalarType* mean = (ScalarType*)calloc(D, sizeof(ScalarType));
        if (mean == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        for (int n = 0; n < N; n++)
        {
            for (int d = 0; d < D; d++)
            {
                mean[d] += X[n * D + d];
            }
        }
        for (int d = 0; d < D; d++)
        {
            mean[d] /= (ScalarType)N;
        }

        // Subtract data mean
        for (int n = 0; n < N; n++)
        {
            for (int d = 0; d < D; d++)
            {
                X[n * D + d] -= mean[d];
            }
        }
        free(mean);
        mean = NULL;
    }

    void computeGaussianPerplexity(ScalarType* X, int N, int D, ScalarType* P, ScalarType perplexity)
    {
        // Compute the squared Euclidean distance matrix
        ScalarType* DD = (ScalarType*)malloc(static_cast<size_t>(N) * N * sizeof(ScalarType));
        if (DD == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        computeSquaredEuclideanDistance(X, N, D, DD);

        // Compute the Gaussian kernel row by row
        for (int n = 0; n < N; n++)
        {

            // Initialize some variables
            bool found = false;
            ScalarType beta = 1.0;
            ScalarType min_beta = -DBL_MAX;
            ScalarType max_beta = DBL_MAX;
            ScalarType tol = 1e-5;
            ScalarType sum_P;

            // Iterate until we found a good perplexity
            int iter = 0;
            while (!found && iter < 200)
            {

                // Compute Gaussian kernel row
                for (int m = 0; m < N; m++)
                    P[n * N + m] = exp(-beta * DD[n * N + m]);
                P[n * N + n] = DBL_MIN;

                // Compute entropy of current row
                sum_P = DBL_MIN;
                for (int m = 0; m < N; m++)
                    sum_P += P[n * N + m];
                ScalarType H = 0.0;
                for (int m = 0; m < N; m++)
                    H += beta * (DD[n * N + m] * P[n * N + m]);
                H = (H / sum_P) + log(sum_P);

                // Evaluate whether the entropy is within the tolerance level
                ScalarType Hdiff = H - log(perplexity);
                if (Hdiff < tol && -Hdiff < tol)
                {
                    found = true;
                }
                else
                {
                    if (Hdiff > 0)
                    {
                        min_beta = beta;
                        if (max_beta == DBL_MAX || max_beta == -DBL_MAX)
                            beta *= 2.0;
                        else
                            beta = (beta + max_beta) / 2.0;
                    }
                    else
                    {
                        max_beta = beta;
                        if (min_beta == -DBL_MAX || min_beta == DBL_MAX)
                            beta /= 2.0;
                        else
                            beta = (beta + min_beta) / 2.0;
                    }
                }

                // Update iteration counter
                iter++;
            }

            // Row normalize P
            for (int m = 0; m < N; m++)
                P[n * N + m] /= sum_P;
        }

        // Clean up memory
        free(DD);
        DD = NULL;
    }

    void computeGaussianPerplexity(ScalarType* X, int N, int D, int** _row_P, int** _col_P, ScalarType** _val_P,
                                   ScalarType perplexity, int K)
    {
        if (perplexity > K)
            printf("Perplexity should be lower than K!\n");

        // Allocate the memory we need
        *_row_P = (int*)malloc((N + 1) * sizeof(int));
        *_col_P = (int*)calloc(static_cast<size_t>(N) * K, sizeof(int));
        *_val_P = (ScalarType*)calloc(static_cast<size_t>(N) * K, sizeof(ScalarType));
        if (*_row_P == NULL || *_col_P == NULL || *_val_P == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        int* row_P = *_row_P;
        int* col_P = *_col_P;
        ScalarType* val_P = *_val_P;
        ScalarType* cur_P = (ScalarType*)malloc((N - 1) * sizeof(ScalarType));
        if (cur_P == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        row_P[0] = 0;
        for (int n = 0; n < N; n++)
            row_P[n + 1] = row_P[n] + K;

        // Build ball tree on data set
        VpTree<DataPoint, euclidean_distance>* tree = new VpTree<DataPoint, euclidean_distance>();
        std::vector<DataPoint> obj_X(N, DataPoint(D, -1, X));
        for (int n = 0; n < N; n++)
            obj_X[n] = DataPoint(D, n, X + n * D);
        tree->create(obj_X);

        // Loop over all points to find nearest neighbors
        // printf("Building tree...\n");
        std::vector<DataPoint> indices;
        std::vector<ScalarType> distances;
        for (int n = 0; n < N; n++)
        {

            // if(n % 10000 == 0) printf(" - point %d of %d\n", n, N);

            // Find nearest neighbors
            indices.clear();
            distances.clear();
            tree->search(obj_X[n], K + 1, &indices, &distances);

            // Initialize some variables for binary search
            bool found = false;
            ScalarType beta = 1.0;
            ScalarType min_beta = -DBL_MAX;
            ScalarType max_beta = DBL_MAX;
            ScalarType tol = 1e-5;

            // Iterate until we found a good perplexity
            int iter = 0;
            ScalarType sum_P;
            while (!found && iter < 200)
            {

                // Compute Gaussian kernel row
                for (int m = 0; m < K; m++)
                    cur_P[m] = exp(-beta * distances[m + 1]);

                // Compute entropy of current row
                sum_P = DBL_MIN;
                for (int m = 0; m < K; m++)
                    sum_P += cur_P[m];
                ScalarType H = .0;
                for (int m = 0; m < K; m++)
                    H += beta * (distances[m + 1] * cur_P[m]);
                H = (H / sum_P) + log(sum_P);

                // Evaluate whether the entropy is within the tolerance level
                ScalarType Hdiff = H - log(perplexity);
                if (Hdiff < tol && -Hdiff < tol)
                {
                    found = true;
                }
                else
                {
                    if (Hdiff > 0)
                    {
                        min_beta = beta;
                        if (max_beta == DBL_MAX || max_beta == -DBL_MAX)
                            beta *= 2.0;
                        else
                            beta = (beta + max_beta) / 2.0;
                    }
                    else
                    {
                        max_beta = beta;
                        if (min_beta == -DBL_MAX || min_beta == DBL_MAX)
                            beta /= 2.0;
                        else
                            beta = (beta + min_beta) / 2.0;
                    }
                }

                // Update iteration counter
                iter++;
            }

            // Row-normalize current row of P and store in matrix
            for (int m = 0; m < K; m++)
                cur_P[m] /= sum_P;
            for (int m = 0; m < K; m++)
            {
                col_P[row_P[n] + m] = indices[m + 1].index();
                val_P[row_P[n] + m] = cur_P[m];
            }
        }

        // Clean up memory
        obj_X.clear();
        free(cur_P);
        delete tree;
    }

    void computeGaussianPerplexity(ScalarType* X, int N, int D, int** _row_P, int** _col_P, ScalarType** _val_P,
                                   ScalarType perplexity, ScalarType threshold)
    {
        // Allocate some memory we need for computations
        ScalarType* buff = (ScalarType*)malloc(static_cast<size_t>(D) * sizeof(ScalarType));
        ScalarType* DD = (ScalarType*)malloc(static_cast<size_t>(N) * sizeof(ScalarType));
        ScalarType* cur_P = (ScalarType*)malloc(static_cast<size_t>(N) * sizeof(ScalarType));
        if (buff == NULL || DD == NULL || cur_P == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }

        // Compute the Gaussian kernel row by row (to find number of elements in sparse P)
        int total_count = 0;
        for (int n = 0; n < N; n++)
        {

            // Compute the squared Euclidean distance matrix
            for (int m = 0; m < N; m++)
            {
                for (int d = 0; d < D; d++)
                    buff[d] = X[n * D + d];
                for (int d = 0; d < D; d++)
                    buff[d] -= X[m * D + d];
                DD[m] = .0;
                for (int d = 0; d < D; d++)
                    DD[m] += buff[d] * buff[d];
            }

            // Initialize some variables
            bool found = false;
            ScalarType beta = 1.0;
            ScalarType min_beta = -DBL_MAX;
            ScalarType max_beta = DBL_MAX;
            ScalarType tol = 1e-5;

            // Iterate until we found a good perplexity
            int iter = 0;
            ScalarType sum_P;
            while (!found && iter < 200)
            {

                // Compute Gaussian kernel row
                for (int m = 0; m < N; m++)
                    cur_P[m] = exp(-beta * DD[m]);
                cur_P[n] = DBL_MIN;

                // Compute entropy of current row
                sum_P = DBL_MIN;
                for (int m = 0; m < N; m++)
                    sum_P += cur_P[m];
                ScalarType H = 0.0;
                for (int m = 0; m < N; m++)
                    H += beta * (DD[m] * cur_P[m]);
                H = (H / sum_P) + log(sum_P);

                // Evaluate whether the entropy is within the tolerance level
                ScalarType Hdiff = H - log(perplexity);
                if (Hdiff < tol && -Hdiff < tol)
                {
                    found = true;
                }
                else
                {
                    if (Hdiff > 0)
                    {
                        min_beta = beta;
                        if (max_beta == DBL_MAX || max_beta == -DBL_MAX)
                            beta *= 2.0;
                        else
                            beta = (beta + max_beta) / 2.0;
                    }
                    else
                    {
                        max_beta = beta;
                        if (min_beta == -DBL_MAX || min_beta == DBL_MAX)
                            beta /= 2.0;
                        else
                            beta = (beta + min_beta) / 2.0;
                    }
                }

                // Update iteration counter
                iter++;
            }

            // Row-normalize and threshold current row of P
            for (int m = 0; m < N; m++)
                cur_P[m] /= sum_P;
            for (int m = 0; m < N; m++)
            {
                if (cur_P[m] > threshold / (ScalarType)N)
                    total_count++;
            }
        }

        // Allocate the memory we need
        *_row_P = (int*)malloc((N + 1) * sizeof(int));
        *_col_P = (int*)malloc(total_count * sizeof(int));
        *_val_P = (ScalarType*)malloc(total_count * sizeof(ScalarType));
        int* row_P = *_row_P;
        int* col_P = *_col_P;
        ScalarType* val_P = *_val_P;
        if (row_P == NULL || col_P == NULL || val_P == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        row_P[0] = 0;

        // Compute the Gaussian kernel row by row (this time, store the results)
        int count = 0;
        for (int n = 0; n < N; n++)
        {

            // Compute the squared Euclidean distance matrix
            for (int m = 0; m < N; m++)
            {
                for (int d = 0; d < D; d++)
                    buff[d] = X[n * D + d];
                for (int d = 0; d < D; d++)
                    buff[d] -= X[m * D + d];
                DD[m] = .0;
                for (int d = 0; d < D; d++)
                    DD[m] += buff[d] * buff[d];
            }

            // Initialize some variables
            bool found = false;
            ScalarType beta = 1.0;
            ScalarType min_beta = -DBL_MAX;
            ScalarType max_beta = DBL_MAX;
            ScalarType tol = 1e-5;

            // Iterate until we found a good perplexity
            int iter = 0;
            ScalarType sum_P;
            while (!found && iter < 200)
            {

                // Compute Gaussian kernel row
                for (int m = 0; m < N; m++)
                    cur_P[m] = exp(-beta * DD[m]);
                cur_P[n] = DBL_MIN;

                // Compute entropy of current row
                sum_P = DBL_MIN;
                for (int m = 0; m < N; m++)
                    sum_P += cur_P[m];
                ScalarType H = 0.0;
                for (int m = 0; m < N; m++)
                    H += beta * (DD[m] * cur_P[m]);
                H = (H / sum_P) + log(sum_P);

                // Evaluate whether the entropy is within the tolerance level
                ScalarType Hdiff = H - log(perplexity);
                if (Hdiff < tol && -Hdiff < tol)
                {
                    found = true;
                }
                else
                {
                    if (Hdiff > 0)
                    {
                        min_beta = beta;
                        if (max_beta == DBL_MAX || max_beta == -DBL_MAX)
                            beta *= 2.0;
                        else
                            beta = (beta + max_beta) / 2.0;
                    }
                    else
                    {
                        max_beta = beta;
                        if (min_beta == -DBL_MAX || min_beta == DBL_MAX)
                            beta /= 2.0;
                        else
                            beta = (beta + min_beta) / 2.0;
                    }
                }

                // Update iteration counter
                iter++;
            }

            // Row-normalize and threshold current row of P
            for (int m = 0; m < N; m++)
                cur_P[m] /= sum_P;
            for (int m = 0; m < N; m++)
            {
                if (cur_P[m] > threshold / (ScalarType)N)
                {
                    col_P[count] = m;
                    val_P[count] = cur_P[m];
                    count++;
                }
            }
            row_P[n + 1] = count;
        }

        // Clean up memory
        free(DD);
        DD = NULL;
        free(buff);
        buff = NULL;
        free(cur_P);
        cur_P = NULL;
    }

    void computeSquaredEuclideanDistance(ScalarType* X, int N, int D, ScalarType* DD)
    {
        ScalarType* dataSums = (ScalarType*)calloc(N, sizeof(ScalarType));
        if (dataSums == NULL)
        {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        for (int n = 0; n < N; n++)
        {
            for (int d = 0; d < D; d++)
            {
                dataSums[n] += (X[n * D + d] * X[n * D + d]);
            }
        }
        for (int n = 0; n < N; n++)
        {
            for (int m = 0; m < N; m++)
            {
                DD[n * N + m] = dataSums[n] + dataSums[m];
            }
        }
        Eigen::Map<tapkee::DenseMatrix> DD_map(DD, N, N);
        Eigen::Map<tapkee::DenseMatrix> X_map(X, D, N);
        DD_map.noalias() = -2.0 * X_map.transpose() * X_map;

        // cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans, N, N, D, -2.0, X, D, X, D, 1.0, DD, N);
        free(dataSums);
        dataSums = NULL;
    }
};

} // namespace tsne
