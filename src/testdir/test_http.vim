" Tests for httprequest()

func Test_httprequest_file_scheme()
  CheckFeature http

  let tmpfile = tempname()
  call writefile(['hello from file'], tmpfile)

  let abs = substitute(fnamemodify(tmpfile, ':p'), '\\', '/', 'g')
  let url = has('win32') ? 'file:///' .. abs : 'file://' .. abs

  try
    let res = httprequest('GET', url, v:null)
    call assert_equal(1, res.success)
    call assert_equal(0, res.status)
    call assert_equal("hello from file\n", res.body)
    call assert_equal(v:t_dict, type(res.headers))
  finally
    call delete(tmpfile)
  endtry
endfunc

func Test_httprequest_invalid_header_type()
  CheckFeature http
  call assert_fails("call httprequest('GET', '', #{foo: 123})",
        \ 'E474: Invalid argument: header values must be string or list of string')
endfunc

" vim: shiftwidth=2 sts=2 expandtab
