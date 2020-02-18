// #include <Rcpp.h>
#include <RcppArmadillo.h>
#include "beachmat/numeric_matrix.h"
#include "beachmat/integer_matrix.h"


using namespace Rcpp;


// [[Rcpp::export]]
double compute_gp_deviance (double y, double mu, double theta) {
  if(theta < 1e-6){
    // If theta is so small, calculate Poisson deviance
    if(y == 0){
      return 2.0 * mu;
    }else{
      return 2.0 * (y * std::log(y/mu) - (y - mu));
    }
  }else{
    // Otherwise calculate Gamma-Poisson deviance
    if(y == 0){
      return 2.0/theta * std::log((1 + mu * theta));
    } else {
      double s1 = y * std::log((mu + y * mu * theta) / (y +  y * mu * theta));
      double s2 = 1.0/theta * std::log((1 + mu * theta) / (1 + y * theta));
      return -2.0 * (s1 - s2);
    }
  }
}

template<class NumericType>
double compute_gp_deviance_sum(const arma::Mat<NumericType>& Y,
                               const arma::Mat<double>& Mu,
                               const NumericVector& thetas){
  double dev = 0.0;
  int nrows = Y.n_rows;
  for (int i = 0; i < Y.n_elem; i++) {
    dev += compute_gp_deviance(Y.at(i), Mu.at(i), thetas(i % nrows));
  }
  return dev;
}

template<class NumericType>
double compute_gp_deviance_sum(const arma::Mat<NumericType>& Y,
                               const arma::Mat<double>& Mu,
                               double theta){
  double dev = 0.0;
  for (int i = 0; i < Y.n_elem; i++) {
    dev += compute_gp_deviance(Y.at(i), Mu.at(i), theta);
  }
  return dev;
}



template<class NumericType>
void clamp_inplace(arma::Mat<NumericType>& v, double min, double max){
  for(int i = 0; i < v.n_elem; i++){
    if(v.at(i) < min){
      v.at(i) = min;
    }else if(v.at(i) > max){
      v.at(i) = max;
    }
  }
}



//--------------------------------------------------------------------------------------------------//
// The following code was originally copied from https://github.com/mikelove/DESeq2/blob/master/src/DESeq2.cpp
// I adapted it to the needs of this project by:
//  * remove lambda / ridge penality
//  * remove weights
//  * Calculate actual deviance (2 * (log(f_NB(y | mu, theta)) - log(f_NB(y | y, theta))))
//    instead of just 2 * log(f_NB(y | mu, theta)),
//  * Support DelayedArrays
//  * Remove unncessary outputs: beta_mat_var, hat_diagonals, deviance
//  * Remove beta divergence check if abs(beta) very large
//  * Add line search that ensures that deviance is decreasing at every step


// fit the Negative Binomial GLM with Fisher scoring
// note: the betas are on the natural log scale
//
template<class NumericType, class BMNumericType>
List fitBeta_fisher_scoring_impl(RObject Y, const arma::mat& model_matrix, RObject exp_offset_matrix,
                                 NumericVector thetas, SEXP beta_matSEXP,
                                 double tolerance, int max_iter) {
  auto Y_bm = beachmat::create_matrix<BMNumericType>(Y);
  auto exp_offsets_bm = beachmat::create_numeric_matrix(exp_offset_matrix);
  int n_samples = Y_bm->get_ncol();
  int n_genes = Y_bm->get_nrow();

  // The result
  arma::mat beta_mat = as<arma::mat>(beta_matSEXP);
  // The QR decomposition of the model_matrix
  arma::mat q, r;
  // deviance, convergence and tolerance
  double dev, dev_old, speeding_factor, conv_test;


  NumericVector iterations(n_genes);
  NumericVector deviance(n_genes);
  for (int gene_idx = 0; gene_idx < n_genes; gene_idx++) {
    if (gene_idx % 100 == 0) checkUserInterrupt();
    arma::Row<NumericType> counts(n_samples);
    Y_bm->get_row(gene_idx, counts.begin());
    arma::Row<double> exp_off(n_samples);
    exp_offsets_bm->get_row(gene_idx, exp_off.begin());


    arma::Row<double> beta_hat = beta_mat.row(gene_idx);
    arma::Row<double> mu_hat = exp_off % exp(beta_hat * model_matrix.t());
    clamp_inplace(mu_hat, 1e-50, 1e50);

    dev = compute_gp_deviance_sum(counts, mu_hat, thetas(gene_idx));
    dev_old = dev;
    speeding_factor = 1.0;

    // make an orthonormal design matrix
    for (int t = 0; t < max_iter; t++) {
      iterations(gene_idx)++;
      arma::vec w_vec = (mu_hat/(1.0 + thetas(gene_idx) * mu_hat)).t();
      arma::vec w_sqrt_vec = sqrt(w_vec);

      // prepare matrices
      arma::mat weighted_model_matrix = model_matrix.each_col() % w_sqrt_vec;
      qr_econ(q, r, weighted_model_matrix);
      // Not actually quite the score vec, but related
      // See Dunn&Smyth GLM Book eq. 6.16
      arma::rowvec score_vec = ((counts - mu_hat) / mu_hat) * (q.each_col() % w_sqrt_vec);
      arma::rowvec step = solve(arma::trimatu(r), score_vec.t()).t();

      // Find speedfactor that actually decreases the deviance
      arma::Row<double> beta_prop;
      int line_iter = 0;
      while(true){
        beta_prop = beta_hat + speeding_factor * step;
        mu_hat = exp_off % exp(beta_prop * model_matrix.t());
        clamp_inplace(mu_hat, 1e-50, 1e50);
        dev = compute_gp_deviance_sum(counts, mu_hat, thetas(gene_idx));
        conv_test = fabs(dev - dev_old)/(fabs(dev) + 0.1);
        if(dev < dev_old || conv_test < tolerance){
          break; // while loop
        }else if(line_iter >= 100){
          // speeding factor is very small, something is going wrong here
          conv_test = std::numeric_limits<double>::quiet_NaN();
          break; // while loop
        }else{
          // Halfing the speed
          speeding_factor = speeding_factor / 2.0;
        }
        line_iter++;
      }
      if(line_iter == 0 && speeding_factor < 1.0){
        // If step is directly accepted, increase speeding_factor
        // slowly up to full speed = 1.0
        speeding_factor = std::min(speeding_factor * 1.5, 1.0);
      }
      beta_hat = beta_prop;

      if (std::isnan(conv_test)) {
        beta_hat.fill(NA_REAL);
        iterations(gene_idx) = max_iter;
        break;
      }
      if ((t > 0) & (conv_test < tolerance)) {
        break;
      }
      dev_old = dev;
    }

    beta_mat.row(gene_idx) = beta_hat;
  }

  return List::create(
    Named("beta_mat", beta_mat),
    Named("iter", iterations));
}


// [[Rcpp::export]]
List fitBeta_fisher_scoring(RObject Y, const arma::mat& model_matrix, RObject exp_offset_matrix,
                                  NumericVector thetas, SEXP beta_matSEXP,
                                  double tolerance, int max_iter) {
  auto mattype=beachmat::find_sexp_type(Y);
  if (mattype==INTSXP) {
    return fitBeta_fisher_scoring_impl<int, beachmat::integer_matrix>(Y, model_matrix, exp_offset_matrix, thetas,  beta_matSEXP, tolerance, max_iter);
  } else if (mattype==REALSXP) {
    return fitBeta_fisher_scoring_impl<double, beachmat::numeric_matrix>(Y, model_matrix, exp_offset_matrix, thetas,  beta_matSEXP, tolerance, max_iter);
  } else {
    throw std::runtime_error("unacceptable matrix type");
  }
}



// If there is only one group, there is no need to do the full Fisher-scoring
// Instead a simple Newton-Raphson algorithm will do
template<class NumericType>
List fitBeta_one_group_internal(SEXP Y_SEXP, SEXP offsets_SEXP,
                       NumericVector thetas, NumericVector beta_start_values,
                       double tolerance, int maxIter) {
  auto Y_bm = beachmat::create_matrix<NumericType>(Y_SEXP);

  auto offsets_bm = beachmat::create_numeric_matrix(offsets_SEXP);
  int n_samples = Y_bm->get_ncol();
  int n_genes = Y_bm->get_nrow();
  NumericVector result(n_genes);
  IntegerVector iterations(n_genes);

  for(int gene_idx = 0; gene_idx < n_genes; gene_idx++){
    if (gene_idx % 100 == 0) checkUserInterrupt();

    double beta = beta_start_values(gene_idx);
    const double& theta = thetas(gene_idx);

    typename NumericType::vector counts(n_samples);
    Y_bm->get_row(gene_idx, counts.begin());
    NumericVector off(n_samples);
    offsets_bm->get_row(gene_idx, off.begin());
    // Newton-Raphson
    int iter = 0;
    for(; iter < maxIter; iter++){
      double dl = 0.0;
      double ddl = 0.0;
      bool all_zero = true;
      for(int sample_iter = 0; sample_iter < n_samples; sample_iter++){
        const auto count = counts[sample_iter];
        all_zero = all_zero && count == 0;
        const double mu = std::exp(beta + off[sample_iter]);
        const double denom = 1.0 + mu * theta;
        dl += (count - mu) / denom;
        ddl += mu * (1.0 + count * theta) / denom / denom;
        // ddl += mu / denom;           // This is what edgeR is using
      }
      if(all_zero){
        beta = R_NegInf;
        break;
      }
      const double step = dl / ddl;
      beta += step;
      if(abs(step) < tolerance){
        break;
      }
    }
    result(gene_idx) = beta;
    iterations(gene_idx) = iter;
  }
  return List::create(
    Named("beta", result),
    Named("iter", iterations)
  );
}

// [[Rcpp::export(rng = false)]]
List fitBeta_one_group(RObject Y, RObject offset_matrix,
                        NumericVector thetas, NumericVector beta_start_values,
                        double tolerance, int maxIter) {
  auto mattype=beachmat::find_sexp_type(Y);
  if (mattype==INTSXP) {
    return fitBeta_one_group_internal<beachmat::integer_matrix>(Y, offset_matrix, thetas, beta_start_values, tolerance, maxIter);
  } else if (mattype==REALSXP) {
    return fitBeta_one_group_internal<beachmat::numeric_matrix>(Y, offset_matrix, thetas, beta_start_values, tolerance, maxIter);
  } else {
    throw std::runtime_error("unacceptable matrix type");
  }
}







// fit the Negative Binomial GLM with a diagonal approximation of Fisher scoring
// This is helpful if the model_matrix has very many coefficients (p). The classical
// algorithm needs an inversion of a matrix with size p x p.
// This algorithm is linear in p.
// This is achieved by ignoring the mixed second derivatives of information matrix.
// For a more detailed explanation see: Townes, 2019: Generalized Principal Component Analysis
// note: the code is a direct adapation of the fitBeta_fisher_scoring_impl algorithm
template<class NumericType, class BMNumericType>
List fitBeta_diagonal_fisher_scoring_impl(RObject Y, const arma::mat& model_matrix, RObject exp_offset_matrix,
                                 NumericVector thetas, SEXP beta_matSEXP,
                                 double tolerance, int max_iter) {
  auto Y_bm = beachmat::create_matrix<BMNumericType>(Y);
  auto exp_offsets_bm = beachmat::create_numeric_matrix(exp_offset_matrix);
  int n_samples = Y_bm->get_ncol();
  int n_genes = Y_bm->get_nrow();

  // The result
  arma::mat beta_mat = as<arma::mat>(beta_matSEXP);
  // deviance, convergence and tolerance
  double dev, dev_old, speeding_factor, conv_test;

  // Declare beta diverged if abs larger
  // and use value from previous iteration
  // In DESeq2 diverged beta values are re-calculated using optim
  // Currently not in use.
  // double beta_divergence_threshold = 3000.0;

  NumericVector iterations(n_genes);
  NumericVector deviance(n_genes);
  for (int gene_idx = 0; gene_idx < n_genes; gene_idx++) {
    if (gene_idx % 100 == 0) checkUserInterrupt();
    arma::Row<NumericType> counts(n_samples);
    Y_bm->get_row(gene_idx, counts.begin());
    arma::Row<double> exp_off(n_samples);
    exp_offsets_bm->get_row(gene_idx, exp_off.begin());


    arma::Row<double> beta_hat = beta_mat.row(gene_idx);
    arma::Row<double> mu_hat = exp_off % exp(beta_hat * model_matrix.t());
    clamp_inplace(mu_hat, 1e-50, 1e50);

    dev = compute_gp_deviance_sum(counts, mu_hat, thetas(gene_idx));
    dev_old = dev;
    speeding_factor = 1.0;

    // make an orthonormal design matrix
    for (int t = 0; t < max_iter; t++) {
      iterations(gene_idx)++;
      arma::vec w_vec = (mu_hat/(1.0 + thetas(gene_idx) * mu_hat)).t();

      // prepare matrices
      arma::mat weighted_model_matrix = model_matrix.each_col() % w_vec;
      arma::rowvec score_vec = ((counts - mu_hat) / mu_hat) * weighted_model_matrix;
      // This calculates the diag(Xˆt W X) efficiently. arma::sum(mat, 0) = colSums()
      arma::rowvec info_vec = arma::sum(arma::mat(arma::pow(model_matrix, 2)).each_col() % w_vec, 0);
      arma::rowvec step = score_vec / info_vec;

      // Find speedfactor that actually decreases the deviance
      arma::Row<double> beta_prop;
      int line_iter = 0;
      while(true){
        beta_prop = beta_hat + speeding_factor * step;
        mu_hat = exp_off % exp(beta_prop * model_matrix.t());
        clamp_inplace(mu_hat, 1e-50, 1e50);
        dev = compute_gp_deviance_sum(counts, mu_hat, thetas(gene_idx));
        conv_test = fabs(dev - dev_old)/(fabs(dev) + 0.1);
        if(dev < dev_old || conv_test < tolerance){
          break; // while loop
        }else if(line_iter >= 100 || speeding_factor < 1e-6){
          // speeding factor is very small, something is going wrong here
          conv_test = std::numeric_limits<double>::quiet_NaN();
          break; // while loop
        }else{
          // Halfing the speed
          speeding_factor = speeding_factor / 2.0;
        }
        line_iter++;
      }
      if(line_iter == 0 && speeding_factor < 1.0){
        // If step is directly accepted, increase speeding_factor
        // slowly up to full speed = 1.0
        speeding_factor = std::min(speeding_factor * 1.5, 1.0);
      }
      beta_hat = beta_prop;

      if (std::isnan(conv_test)) {
        beta_hat.fill(NA_REAL);
        iterations(gene_idx) = max_iter;
        break;
      }
      if ((t > 0) & (conv_test < tolerance)) {
        break;
      }
      dev_old = dev;
    }

    beta_mat.row(gene_idx) = beta_hat;
  }

  return List::create(
    Named("beta_mat", beta_mat),
    Named("iter", iterations));
}


// [[Rcpp::export]]
List fitBeta_diagonal_fisher_scoring(RObject Y, const arma::mat& model_matrix, RObject exp_offset_matrix,
                            NumericVector thetas, SEXP beta_matSEXP,
                            double tolerance, int max_iter) {
  auto mattype=beachmat::find_sexp_type(Y);
  if (mattype==INTSXP) {
    return fitBeta_diagonal_fisher_scoring_impl<int, beachmat::integer_matrix>(Y, model_matrix, exp_offset_matrix, thetas,  beta_matSEXP, tolerance, max_iter);
  } else if (mattype==REALSXP) {
    return fitBeta_diagonal_fisher_scoring_impl<double, beachmat::numeric_matrix>(Y, model_matrix, exp_offset_matrix, thetas,  beta_matSEXP, tolerance, max_iter);
  } else {
    throw std::runtime_error("unacceptable matrix type");
  }
}




