import java.io.*;  
public class TestExec {  
    public static void main(String[] args) {  
        try {  
            Process p = Runtime.getRuntime().exec("/usr/bin/java TestExecLs");  
            BufferedReader in = new BufferedReader(  
                                new InputStreamReader(p.getInputStream()));  
            String line = null;  
            while ((line = in.readLine()) != null) {  
                System.out.println(line);  
            }  
        } catch (IOException e) {  
            e.printStackTrace();  
        }  
    }  
}
