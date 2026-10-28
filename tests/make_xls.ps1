# Makes tests/corpus/sample.xls from sample.xlsx with Excel itself (dev machine with Excel only;
# the .xls is committed). Excel runs invisibly and is closed afterwards.
$corpus = Join-Path $PSScriptRoot "corpus"
$excel = New-Object -ComObject Excel.Application
$excel.Visible = $false
$excel.DisplayAlerts = $false
try {
    $wb = $excel.Workbooks.Open((Join-Path $corpus "sample.xlsx"))
    $wb.SaveAs((Join-Path $corpus "sample.xls"), 56)   # 56: Excel 97-2003 workbook
    $wb.Close($false)
} finally {
    $excel.Quit()
    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($excel)
}
