
# This function was copied from proDA
parse_contrast <- function(contrast, levels) {
  cnt_capture <- substitute(contrast)
  parse_contrast_q(cnt_capture, levels, env = parent.frame())
}


parse_contrast_q <- function(contrast, levels, env = parent.frame()) {
  if(missing(contrast)){
    stop("No contrast argument was provided! The option is any linear combination of:\n",
         paste0(levels, collapse = ", "))
  }

  stopifnot(! is.null(levels))
  if(is.factor(levels)){
    levels <- levels(levels)
  }else if(! is.character(levels)){
    stop("levels must be either a character vector or a factor")
  }

  indicators <- diag(nrow=length(levels))
  rownames(indicators) <- levels
  colnames(indicators) <- levels

  level_environment <- new.env(parent = env)

  for(lvl in levels){
    ind <- indicators[, lvl]
    names(ind) <- levels
    assign(lvl, ind, level_environment)
  }
  tryCatch({
    res <- eval(contrast, envir= level_environment)
    if(! is.numeric(res)){
      if(is.character(res)){
        # If contrast was a string, eval will just spit it out the same way
        res <- eval(parse(text = res), envir= level_environment)
      }
    }
  }, error = function(e){
    # Try to extract text from error message
    match <- regmatches(e$message, regexec("object '(.+)' not found", e$message))[[1]]
    if(length(match) == 2){
      stop("Object '", match[2], "' not found. Allowed variables in contrast are:\n",
           paste0(levels, collapse = ", "))
    }else{
      stop(e$message)
    }
  })
  res
}




