mod_upload
----------

mod_upload is an input filter module for multipart/form-data, as submitted from File Upload forms on the Web. mod_upload decodes the data, so the handler gets the file itself without the MIME encoding. Other fields from the form are provided as a table of names/values.

A second filter, formerly mod_tmpfile, is available to store the file contents in a tempfile. This ensures that all the Form data are available when the file is processed, and is useful for handlers that need to run synchronously.
Example

Consider a simple HTML file upload fragment:


<form method="post" action="my-handler" enctype="multipart/form-data">
Your name: <input name="name">
Your email address: <input name="email">
<br>File: <input name="file" type="file">
<br>Additional comments <textarea rows=4 cols=40 name="comments">
</textarea>
</form>

This will generate an HTTP POST request containing an uploaded file with three additional user-supplied text fields. The data are MIME-encoded and normally the Handler has to decode them.
upload-filter

Using upload-filter as an input filter, the handler will be passed instead the decoded file contents. It can access the other Form data (the values of name, email and comments through a table exported by the function
apr_table_t* mod_upload_form(request_rec* r)
which is available to any Handler or other module that needs the data.
Synchronous Upload

In the above example, the handler receives the upload asynchronously. So for example, if a browser passes Form fields in the order they appear in the HTML Form, then when processing the file starts, the values of name and email are available, but the value of comments may not yet have reached the server.
tmpfile-filter

For applications requiring synchronous data, a second filter tmpfile-filter is provided. tmpfile-filter ensures that the upload is complete before data are passed through to the handler. To avoid the risk of overloading memory with a big upload, it saves the file to a tempfile and passes only the filename through to the handler.
Configuration

To insert mod_upload, use the AddInputFilter or SetInputFilter directives. For pipelined applications,
SetInputFilter upload-filter
or for synchronous applications, add both upload and tmpfile
SetInputFilter tmpfile-filter;upload-filter

mod_upload defines two further configuration directives:

UploadField
    The name of the file upload field in the HTML form to be processed
UploadFormSize
    The size of the table allocated for form elements other than the upload. This is advisory; the APR can handle automatic resizing of tables. 